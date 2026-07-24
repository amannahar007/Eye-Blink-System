'use client'
import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import { Utensils, Users, Droplets, Navigation, Sun, Moon, WifiOff, HelpCircle } from 'lucide-react';

interface Option {
    id: string;
    label: string;
    icon: React.ReactNode;
    color: string;
    lightColor: string;
    soundFile: string;
}
// Define types for Bluetooth objects
interface BluetoothDevice extends EventTarget {
    gatt?: BluetoothRemoteGATTServer;
}
declare global {
    interface Navigator {
        bluetooth: {
            requestDevice(options: {
                filters: Array<{ name?: string; services?: string[] }>;
                optionalServices?: string[];
            }): Promise<BluetoothDevice>;
        };
    }
}
interface BluetoothRemoteGATTServer {
    connect(): Promise<BluetoothRemoteGATTServer>;
    disconnect(): void;
    connected: boolean;
    getPrimaryService(service: string): Promise<BluetoothRemoteGATTService>;
}

interface BluetoothRemoteGATTService {
    getCharacteristic(characteristic: string): Promise<BluetoothRemoteGATTCharacteristic>;
}

interface BluetoothRemoteGATTCharacteristic extends EventTarget {
    startNotifications(): Promise<void>;
    stopNotifications(): Promise<void>;
    readValue(): Promise<DataView>;
    writeValue(value: BufferSource): Promise<void>;
    value?: DataView;
}
const CommunicationInterface: React.FC = () => {
    // Default to light aesthetic per user request
    const [isDarkMode, setIsDarkMode] = useState(false);
    const [isConnected, setIsConnected] = useState(false);
    const [selectedOption, setSelectedOption] = useState<string | null>(null);
    const [device, setDevice] = useState<BluetoothDevice | null>(null);
    const [activeSelection, setActiveSelection] = useState<string | null>(null);
    const [currentMenuIndex, setCurrentMenuIndex] = useState(0);
    const [menuActive, setMenuActive] = useState(false);

    // Memoize options to prevent unnecessary re-renders
    const options: Option[] = useMemo(() => [
        {
            id: 'food',
            label: 'Food',
            icon: <Utensils size={48} strokeWidth={1.5} />,
            color: 'bg-orange-500',
            lightColor: 'bg-orange-100 text-orange-600',
            soundFile: 'food.mp3'
        },
        {
            id: 'help',
            label: 'Help',
            icon: <HelpCircle size={48} strokeWidth={1.5} />,
            color: 'bg-red-500',
            lightColor: 'bg-red-100 text-red-600',
            soundFile: 'help.mp3'
        },
        {
            id: 'outing',
            label: 'Outing',
            icon: <Users size={48} strokeWidth={1.5} />,
            color: 'bg-blue-500',
            lightColor: 'bg-blue-100 text-blue-600',
            soundFile: 'outing.mp3'
        },
        {
            id: 'television',
            label: 'Television',
            icon: <div className="text-4xl">📺</div>,
            color: 'bg-indigo-500',
            lightColor: 'bg-indigo-100 text-indigo-600',
            soundFile: 'television.mp3'
        },
        {
            id: 'washroom',
            label: 'Washroom',
            icon: <Navigation size={48} strokeWidth={1.5} />,
            color: 'bg-purple-500',
            lightColor: 'bg-purple-100 text-purple-600',
            soundFile: 'washroom.mp3'
        },
        {
            id: 'water',
            label: 'Water',
            icon: <Droplets size={48} strokeWidth={1.5} />,
            color: 'bg-cyan-500',
            lightColor: 'bg-cyan-100 text-cyan-600',
            soundFile: 'water.mp3'
        }
    ], []);

    const playSound = useCallback((soundFile: string) => {
        try {
            const audio = new Audio(`./sounds/${soundFile}`);
            audio.volume = 0.7;
            audio.play().catch(error => {
                console.log('Audio play failed:', error);
            });
        } catch (error) {
            console.log('Audio creation failed:', error);
        }
    }, []);

    // The ESP32 sends compact BLE status bytes; the browser is the audio output
    // for the system-level announcements.
    const speakSystemStatus = useCallback((message: string) => {
        if (typeof window === 'undefined' || !('speechSynthesis' in window)) {
            console.warn('Speech synthesis is not available in this browser.');
            return;
        }

        window.speechSynthesis.cancel();
        const utterance = new SpeechSynthesisUtterance(message);
        utterance.rate = 0.9;
        utterance.pitch = 1.0;
        window.speechSynthesis.speak(utterance);
    }, []);

    const characteristicRef = useRef<BluetoothRemoteGATTCharacteristic | null>(null);

    // Write a small tag+index acknowledgement back to the ESP32 so the Arduino IDE
    // Serial Monitor can show that the web app actually received and executed the
    // command the blink FSM sent — closing the loop between firmware and web.
    const writeAck = useCallback(async (tag: string, value: number) => {
        try {
            const characteristic = characteristicRef.current;
            if (!characteristic) return;
            const payload = new Uint8Array([tag.charCodeAt(0), value]);
            await characteristic.writeValue(payload);
        } catch (error) {
            console.warn('ACK write to ESP32 failed:', error);
        }
    }, []);

    const handleNotifications = useCallback((event: Event) => {
        const target = event.target as BluetoothRemoteGATTCharacteristic;
        const value = target.value;
        if (!value) return;

        // Convert DataView to Uint8Array
        const data = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);

        // Handle different notification types from ESP32 state machine
        if (data.length === 1) {
            // Menu state change (0 = Communication Mode Activated, 127 = Timeout / Deactivated)
            if (data[0] === 0) {
                // Menu activated
                setMenuActive(true);
                setCurrentMenuIndex(1); // Start with first option
                setActiveSelection(null);
                speakSystemStatus('System activated');
                writeAck('x', 1);
            } else if (data[0] === 127) {
                // Menu deactivated
                setMenuActive(false);
                setCurrentMenuIndex(0);
                setActiveSelection(null);
                setSelectedOption(null);
                speakSystemStatus('System inactive');
                writeAck('x', 0);
            }
        } else if (data.length === 2) {
            // Menu selection change
            if (data[0] === 'S'.charCodeAt(0)) {
                // 'S' for selection change (1 blink navigation)
                const newIndex = data[1];
                setMenuActive(true);
                setCurrentMenuIndex(newIndex);
                setActiveSelection(null); // Clear active selection when navigating
                writeAck('s', newIndex);
            } else if (data[0] === 'A'.charCodeAt(0)) {
                // 'A' for activation/selection (2 blinks)
                const selectedIndex = data[1];
                if (selectedIndex > 0 && selectedIndex <= options.length) {
                    const optionId = options[selectedIndex - 1].id;
                    setSelectedOption(optionId);
                    setActiveSelection(optionId); // Set as active selection
                    playSound(options[selectedIndex - 1].soundFile); // Play the full sound file
                    // Keep communication mode active, just reset selection styling after short delay
                    setTimeout(() => {
                        setActiveSelection(null);
                    }, 3000);
                    writeAck('a', selectedIndex);
                }
            }
        }
    }, [options, playSound, speakSystemStatus, writeAck]);

    // Handle menu index changes for visual highlight
    useEffect(() => {
        if (menuActive && currentMenuIndex > 0 && currentMenuIndex <= options.length) {
            // Highlight the current menu option
            const optionId = options[currentMenuIndex - 1].id;
            setSelectedOption(optionId);
            playSound("select.mp3");
        } else if (!menuActive) {
            setSelectedOption(null);
        }
    }, [currentMenuIndex, menuActive, options, playSound]);

    const [isConnecting, setIsConnecting] = useState(false);
    const [connectionError, setConnectionError] = useState<string | null>(null);
    const connectedDeviceRef = useRef<BluetoothDevice | null>(null);

    const connectToDevice = useCallback(async () => {
        try {
            setIsConnecting(true);
            setConnectionError(null);

            if (!navigator.bluetooth || !navigator.bluetooth.requestDevice) {
                throw new Error('Web Bluetooth not supported in this browser. Please use Chrome/Edge.');
            }

            console.log('Requesting Bluetooth Device...');
            const device = await navigator.bluetooth.requestDevice({
                // Filter by the advertised service UUID rather than the device name.
                // The service UUID is always in the primary advertising packet, so
                // this is robust even if a scanner/OS ever fails to pick up the name
                // (which lives in the separate scan-response packet).
                filters: [{ services: ['6910123a-eb0d-4c35-9a60-bebe1dcb549d'] }],
                optionalServices: ['6910123a-eb0d-4c35-9a60-bebe1dcb549d']
            }) as BluetoothDevice;

            connectedDeviceRef.current = device;

            if (!device.gatt) {
                throw new Error('Bluetooth device does not support GATT');
            }

            device.addEventListener('gattserverdisconnected', handleDisconnection);

            console.log('Connecting to GATT Server...');
            // A stalled connect() would otherwise hang the UI on "Connecting..." forever.
            const server = await Promise.race([
                device.gatt.connect(),
                new Promise<never>((_, reject) =>
                    setTimeout(() => reject(new Error('Connection timed out. Make sure the ESP32 is powered on and nearby, then try again.')), 10000)
                )
            ]);

            console.log('Getting Service...');
            const service = await server.getPrimaryService('6910123a-eb0d-4c35-9a60-bebe1dcb549d');

            console.log('Getting Characteristic...');
            const characteristic = await service.getCharacteristic('5f4f1107-7fc1-43b2-a540-0aa1a9f1ce78');

            await characteristic.startNotifications();
            characteristic.addEventListener('characteristicvaluechanged', handleNotifications);
            characteristicRef.current = characteristic;

            setDevice(device);
            setIsConnected(true);
            setIsConnecting(false);

            console.log('Successfully connected');
            return true;
        } catch (error) {
            console.error('Connection failed:', error);
            setIsConnecting(false);
            setIsConnected(false);
            setConnectionError(error instanceof Error ? error.message : 'Connection failed');
            return false;
        }
    }, [handleNotifications]);

    const handleDisconnection = useCallback(() => {
        console.log('Device disconnected');
        setIsConnected(false);
        setMenuActive(false);
        setCurrentMenuIndex(0);
        characteristicRef.current = null;
    }, [setIsConnected, setMenuActive, setCurrentMenuIndex]);

    const disconnectDevice = useCallback(async () => {
        try {
            if (!connectedDeviceRef.current) {
                return;
            }

            const server = connectedDeviceRef.current.gatt;
            if (!server) return;

            if (!server.connected) {
                connectedDeviceRef.current = null;
                setIsConnected(false);
                return;
            }

            const service = await server.getPrimaryService("6910123a-eb0d-4c35-9a60-bebe1dcb549d");
            const dataChar = await service.getCharacteristic("5f4f1107-7fc1-43b2-a540-0aa1a9f1ce78");

            await dataChar.stopNotifications();
            dataChar.removeEventListener("characteristicvaluechanged", handleNotifications);

            server.disconnect();
        } catch (error) {
            console.error("Error during disconnection:", error);
        } finally {
            setDevice(null);
            setIsConnected(false);
            setIsConnecting(false);
            setMenuActive(false);
            setCurrentMenuIndex(0);
            characteristicRef.current = null;
        }
    }, [handleNotifications]);

    const toggleConnection = async () => {
        if (isConnected) {
            disconnectDevice();
        } else {
            await connectToDevice();
        }
    };

    const connectionStatusText = () => {
        if (isConnected) return 'Connected to Sensor';
        if (device && !isConnected) return 'Connecting...';
        return 'Ready to Connect';
    };

    const handleOptionClick = (option: Option) => {
        // Allow clicks even when disconnected for testing aesthetics
        setSelectedOption(selectedOption === option.id ? null : option.id);
        playSound(option.soundFile);
    };

    const toggleTheme = () => {
        setIsDarkMode(!isDarkMode);
    };

    // Refined, human-designed aesthetics using Tailwind utilities
    const themeClasses = {
        background: isDarkMode
            ? 'bg-slate-900'
            : 'bg-[#fafcff]', // Soft blue-tinted white for light mode
        text: isDarkMode ? 'text-white' : 'text-slate-800',
        cardBg: isDarkMode
            ? 'bg-white/5 backdrop-blur-xl border border-white/10 shadow-xl shadow-black/20'
            : 'bg-white border border-slate-200/60 shadow-[0_8px_30px_rgb(0,0,0,0.04)]',
        cardHover: isDarkMode
            ? 'hover:bg-white/10 hover:border-white/20 hover:-translate-y-1'
            : 'hover:shadow-[0_8px_30px_rgb(0,0,0,0.08)] hover:-translate-y-1 hover:border-slate-300',
        textSecondary: isDarkMode ? 'text-slate-300' : 'text-slate-600',
        textMuted: isDarkMode ? 'text-slate-400' : 'text-slate-500',
        glassBg: isDarkMode
            ? 'bg-white/5 backdrop-blur-2xl border border-white/10'
            : 'bg-white/80 backdrop-blur-xl border border-slate-200/60 shadow-[0_4px_20px_rgb(0,0,0,0.03)]'
    };

    return (
        <div className={`min-h-screen min-w-full flex flex-col transition-colors duration-500 ${themeClasses.background} ${themeClasses.text} relative overflow-hidden font-sans`}>
            {/* Very subtle background gradients for light/dark mode instead of garish blobs */}
            <div className="absolute inset-0 overflow-hidden pointer-events-none">
                <div className={`absolute top-0 left-0 w-full h-full transition-opacity duration-1000 ${isDarkMode ? 'opacity-100' : 'opacity-0'}`}>
                    <div className="absolute top-[-20%] left-[-10%] w-[50%] h-[50%] bg-purple-900/20 blur-[120px] rounded-full"></div>
                    <div className="absolute bottom-[-20%] right-[-10%] w-[50%] h-[50%] bg-blue-900/20 blur-[120px] rounded-full"></div>
                </div>
                <div className={`absolute top-0 left-0 w-full h-full transition-opacity duration-1000 ${isDarkMode ? 'opacity-0' : 'opacity-100'}`}>
                    <div className="absolute top-0 right-0 w-[80%] h-[60%] bg-gradient-to-bl from-blue-50/80 to-transparent blur-3xl"></div>
                    <div className="absolute bottom-0 left-0 w-[60%] h-[80%] bg-gradient-to-tr from-purple-50/80 to-transparent blur-3xl"></div>
                </div>
            </div>

            <div className="flex-1 flex flex-col px-4 py-6 sm:px-6 sm:py-8 md:px-10 md:py-10 lg:px-16 lg:py-12 max-w-7xl mx-auto w-full relative z-10">

                {/* Header */}
                <div className="flex flex-col sm:flex-row sm:justify-between sm:items-center mb-8 lg:mb-12 gap-6">
                    <div className="flex flex-col">
                        <h1 className={`text-3xl sm:text-4xl md:text-5xl font-extrabold tracking-tight ${isDarkMode ? 'text-white' : 'text-slate-900'}`}>
                            Neuro<span className="text-purple-500">Speak</span>
                        </h1>
                        <p className={`text-sm sm:text-base font-medium mt-1 ${themeClasses.textMuted}`}>
                            Powered by M2W Technology
                        </p>
                    </div>

                    <div className="flex flex-col sm:flex-row items-stretch sm:items-center gap-4">
                        <button
                            onClick={toggleTheme}
                            className={`p-3 rounded-xl flex items-center justify-center ${themeClasses.glassBg} hover:scale-105 active:scale-95 transition-all duration-300 text-slate-500 dark:text-slate-300`}
                            aria-label="Toggle theme"
                        >
                            {isDarkMode ? <Sun size={20} /> : <Moon size={20} />}
                        </button>

                        <div className={`flex items-center space-x-3 px-4 py-3 rounded-xl ${themeClasses.glassBg} transition-all`}>
                            <div className={`relative flex items-center justify-center w-3 h-3 rounded-full ${isConnected ? 'bg-green-500' : isConnecting ? 'bg-blue-500 animate-pulse' : 'bg-slate-300 dark:bg-slate-600'}`}>
                                {isConnected && <div className="absolute w-full h-full rounded-full bg-green-500 animate-ping opacity-40"></div>}
                            </div>
                            <span className="text-sm font-semibold truncate">
                                {connectionStatusText()}
                            </span>
                        </div>

                        <button
                            onClick={toggleConnection}
                            disabled={isConnecting}
                            className={`px-6 py-3 rounded-xl font-semibold transition-all text-sm
                                ${isConnected
                                    ? 'bg-slate-100 text-slate-700 hover:bg-slate-200 dark:bg-slate-800 dark:text-slate-300 dark:hover:bg-slate-700 border border-slate-200 dark:border-slate-700'
                                    : 'bg-purple-600 hover:bg-purple-700 text-white shadow-lg shadow-purple-500/20'
                                }
                                ${isConnecting ? 'opacity-70 cursor-wait' : 'hover:-translate-y-0.5'}
                            `}
                        >
                            {isConnecting ? 'Connecting...' : isConnected ? 'Disconnect' : 'Connect Device'}
                        </button>
                    </div>
                </div>

                {connectionError && (
                    <div className="mb-6 p-4 rounded-xl bg-red-50 dark:bg-red-900/20 border border-red-200 dark:border-red-800/30 text-red-600 dark:text-red-400 text-sm flex items-center space-x-3">
                        <span className="font-semibold">Connection Error:</span>
                        <span>{connectionError}</span>
                    </div>
                )}

                {/* Communication Mode Banner */}
                <div className={`transition-all duration-500 ease-in-out overflow-hidden ${menuActive ? 'max-h-24 opacity-100 mb-8' : 'max-h-0 opacity-0 mb-0'}`}>
                    <div className={`relative overflow-hidden border rounded-2xl p-4 flex items-center justify-center space-x-3 shadow-lg 
                        ${isDarkMode ? 'bg-purple-900/30 border-purple-500/30 shadow-purple-900/20' : 'bg-purple-50 border-purple-200 shadow-purple-500/10'}`}>
                        <div className={`absolute top-0 left-0 w-1 h-full ${isDarkMode ? 'bg-purple-500' : 'bg-purple-400'}`}></div>
                        <div className={`w-3 h-3 rounded-full animate-pulse ${isDarkMode ? 'bg-purple-400 shadow-[0_0_10px_rgba(192,132,252,0.8)]' : 'bg-purple-500 shadow-[0_0_8px_rgba(168,85,247,0.6)]'}`}></div>
                        <span className={`font-bold text-lg tracking-wide ${isDarkMode ? 'text-purple-300' : 'text-purple-700'}`}>
                            Communication Mode Activated
                        </span>
                    </div>
                </div>

                {/* Menu Grid */}
                <div className="w-full mb-12">
                    <div className="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-6 gap-4 sm:gap-6">
                        {options.map((option, index) => {
                            const isSelected = selectedOption === option.id;
                            const isCurrentMenuOption = menuActive && currentMenuIndex === index + 1;
                            const isActiveSelection = activeSelection === option.id;

                            return (
                                <div
                                    key={option.id}
                                    onClick={() => handleOptionClick(option)}
                                    className={`
                                        aspect-[4/3] lg:aspect-square relative group cursor-pointer transition-all duration-300
                                        ${themeClasses.cardBg} ${themeClasses.cardHover}
                                        rounded-2xl p-4 sm:p-5 border-2 flex flex-col items-center justify-center
                                        ${isSelected ? (isDarkMode ? 'border-purple-500 bg-purple-500/10' : 'border-purple-400 bg-purple-50') : 'border-transparent'}
                                        ${isCurrentMenuOption ? (isDarkMode ? 'ring-2 ring-purple-400/50 ring-offset-2 ring-offset-slate-900' : 'ring-2 ring-purple-400/40 ring-offset-2 ring-offset-white') : ''}
                                        ${isActiveSelection ? 'scale-105 shadow-2xl' : ''}
                                    `}
                                >
                                    <div className={`
                                        p-3 sm:p-4 rounded-xl transition-transform duration-500 group-hover:scale-110 mb-3
                                        ${isDarkMode ? 'bg-white/5' : option.lightColor.split(' ')[0]}
                                        ${isSelected ? (isDarkMode ? 'bg-purple-500/20 text-purple-400' : 'text-purple-600') : (isDarkMode ? 'text-slate-300' : option.lightColor.split(' ')[1])}
                                    `}>
                                        {React.isValidElement(option.icon) && typeof option.icon.type === "function"
                                            ? React.cloneElement(option.icon as React.ReactElement<{ className?: string }>)
                                            : option.icon}
                                    </div>
                                    <h3 className={`
                                        text-sm sm:text-base md:text-lg font-bold text-center
                                        ${isSelected ? (isDarkMode ? 'text-purple-300' : 'text-purple-700') : themeClasses.textSecondary}
                                    `}>
                                        {option.label}
                                    </h3>
                                </div>
                            );
                        })}
                    </div>
                </div>

                <div className="flex-grow"></div>

                {/* Instructions / How It Works */}
                <div className="mt-auto">
                    <h3 className={`text-lg font-bold mb-5 ${themeClasses.textSecondary}`}>
                        NeuroSpeak Gestures
                    </h3>
                    <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4">
                        {[
                            { step: '01', title: 'Activate', desc: '4 consecutive blinks to activate mode.', color: 'bg-blue-500', lightColor: 'bg-blue-50 border-blue-100 text-blue-600' },
                            { step: '02', title: 'Navigate', desc: '1 blink to highlight the next option.', color: 'bg-purple-500', lightColor: 'bg-purple-50 border-purple-100 text-purple-600' },
                            { step: '03', title: 'Select', desc: '2 consecutive blinks to speak selection.', color: 'bg-pink-500', lightColor: 'bg-pink-50 border-pink-100 text-pink-600' },
                            { step: '04', title: 'Deactivate', desc: '4 blinks or 10s timeout to exit.', color: 'bg-emerald-500', lightColor: 'bg-emerald-50 border-emerald-100 text-emerald-600' }
                        ].map((item, index) => (
                            <div
                                key={index}
                                className={`${themeClasses.glassBg} 
                                p-5 rounded-2xl flex flex-col justify-start transition-all hover:scale-[1.02]`}
                            >
                                <div className="flex items-center space-x-3 mb-3">
                                    <div className={`w-8 h-8 rounded-lg flex items-center justify-center font-bold text-sm
                                        ${isDarkMode ? `${item.color} text-white shadow-lg` : `${item.lightColor} border`}
                                    `}>
                                        {item.step}
                                    </div>
                                    <div className="font-bold text-base sm:text-lg">{item.title}</div>
                                </div>
                                <div className={`text-sm ${themeClasses.textMuted} leading-relaxed`}>{item.desc}</div>
                            </div>
                        ))}
                    </div>
                </div>
            </div>
        </div>
    );
};

export default CommunicationInterface;