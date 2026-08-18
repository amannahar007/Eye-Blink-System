'use client'
import React, { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import { 
    Utensils, Users, Droplets, Navigation, Sun, Moon, HelpCircle, 
    Eye, Activity, Database, CheckCircle2, Radio, Sparkles, Flame, RefreshCw
} from 'lucide-react';
import { 
    syncSystemState, 
    syncBlinkDetection, 
    syncSelectedOutput, 
    syncCurrentNavigation, 
    syncConnectionState, 
    initAnalytics,
    RTDB_PATHS,
    rtdb
} from '../lib/firebase';
import { ref, onValue } from 'firebase/database';

interface Option {
    id: string;
    label: string;
    icon: React.ReactNode;
    color: string;
    lightColor: string;
    soundFile: string;
}

interface EventLog {
    id: string;
    time: string;
    title: string;
    type: 'BLINK' | 'SELECT' | 'NAVIGATE' | 'SYSTEM' | 'CONNECT';
    detail: string;
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
    const [lastSelectedOutput, setLastSelectedOutput] = useState<{ id: string; label: string; time: string } | null>(null);
    const [device, setDevice] = useState<BluetoothDevice | null>(null);
    const [activeSelection, setActiveSelection] = useState<string | null>(null);
    const [currentMenuIndex, setCurrentMenuIndex] = useState(0);
    const [menuActive, setMenuActive] = useState(false);

    // Realtime telemetry states
    const [liveBlinkCount, setLiveBlinkCount] = useState<number>(0);
    const [blinkAnimTrigger, setBlinkAnimTrigger] = useState<boolean>(false);
    const [firebaseSynced, setFirebaseSynced] = useState<boolean>(true);
    const [recentEvents, setRecentEvents] = useState<EventLog[]>([]);

    // Memoize options
    const options: Option[] = useMemo(() => [
        {
            id: 'food',
            label: 'Food',
            icon: <Utensils size={44} strokeWidth={1.5} />,
            color: 'bg-orange-500',
            lightColor: 'bg-orange-100 text-orange-600',
            soundFile: 'food.mp3'
        },
        {
            id: 'help',
            label: 'Help',
            icon: <HelpCircle size={44} strokeWidth={1.5} />,
            color: 'bg-red-500',
            lightColor: 'bg-red-100 text-red-600',
            soundFile: 'help.mp3'
        },
        {
            id: 'outing',
            label: 'Outing',
            icon: <Users size={44} strokeWidth={1.5} />,
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
            icon: <Navigation size={44} strokeWidth={1.5} />,
            color: 'bg-purple-500',
            lightColor: 'bg-purple-100 text-purple-600',
            soundFile: 'washroom.mp3'
        },
        {
            id: 'water',
            label: 'Water',
            icon: <Droplets size={44} strokeWidth={1.5} />,
            color: 'bg-cyan-500',
            lightColor: 'bg-cyan-100 text-cyan-600',
            soundFile: 'water.mp3'
        }
    ], []);

    // Helper to log UI event feed
    const addEventLog = useCallback((title: string, type: EventLog['type'], detail: string) => {
        const newLog: EventLog = {
            id: `${Date.now()}-${Math.random()}`,
            time: new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }),
            title,
            type,
            detail
        };
        setRecentEvents(prev => [newLog, ...prev.slice(0, 7)]);
    }, []);

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

    // Text to Speech
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

    // Trigger visual pulse for blink
    const triggerBlinkPulse = useCallback((count: number) => {
        setLiveBlinkCount(count);
        setBlinkAnimTrigger(true);
        setTimeout(() => setBlinkAnimTrigger(false), 600);
    }, []);

    // Initialize Firebase Realtime Database Live Listeners on Mount
    useEffect(() => {
        initAnalytics().catch(() => {});
        addEventLog('Firebase Initialized', 'SYSTEM', 'Connected to neurospeak2 Realtime DB');

        let isInitialLoad = true;

        // 1. Listen for Real-time Blink Detections (from ESP32 Wi-Fi Hotspot)
        const unsubBlink = onValue(ref(rtdb, RTDB_PATHS.BLINK_COUNT), (snapshot) => {
            if (isInitialLoad) return;
            const data = snapshot.val();
            if (data && typeof data.blinkCount === 'number') {
                triggerBlinkPulse(data.blinkCount);
                addEventLog(`Hotspot Blink: ${data.blinkCount}`, 'BLINK', `${data.phase || 'Count'} via Firebase`);
            }
        });

        // 2. Listen for System Activation State
        const unsubSystem = onValue(ref(rtdb, RTDB_PATHS.SYSTEM_STATUS), (snapshot) => {
            if (isInitialLoad) return;
            const data = snapshot.val();
            if (data && typeof data.activated === 'boolean') {
                setMenuActive(data.activated);
                if (data.activated) {
                    setCurrentMenuIndex(1);
                    speakSystemStatus('System activated');
                    addEventLog('System Activated', 'SYSTEM', 'Mode ON via Firebase');
                } else {
                    setCurrentMenuIndex(0);
                    setSelectedOption(null);
                    speakSystemStatus('System inactive');
                    addEventLog('System Inactive', 'SYSTEM', 'Mode OFF via Firebase');
                }
            }
        });

        // 3. Listen for Menu Navigation
        const unsubNav = onValue(ref(rtdb, RTDB_PATHS.CURRENT_NAVIGATION), (snapshot) => {
            if (isInitialLoad) return;
            const data = snapshot.val();
            if (data && typeof data.index === 'number') {
                setMenuActive(true);
                setCurrentMenuIndex(data.index);
                const opt = options[data.index - 1];
                if (opt) {
                    setSelectedOption(opt.id);
                    playSound("select.mp3");
                    addEventLog(`Navigated: ${opt.label}`, 'NAVIGATE', `Slot #${data.index}`);
                }
            }
        });

        // 4. Listen for Selected Output
        const unsubSelect = onValue(ref(rtdb, RTDB_PATHS.SELECTED_OUTPUT), (snapshot) => {
            if (isInitialLoad) return;
            const data = snapshot.val();
            if (data && data.optionId) {
                const opt = options.find(o => o.id === data.optionId);
                if (opt) {
                    setSelectedOption(opt.id);
                    setActiveSelection(opt.id);
                    setLastSelectedOutput({
                        id: opt.id,
                        label: opt.label,
                        time: new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
                    });
                    playSound(opt.soundFile);
                    addEventLog(`Hotspot Output: ${opt.label}`, 'SELECT', 'Triggered via Firebase');
                    setTimeout(() => setActiveSelection(null), 3000);
                }
            }
        });

        setTimeout(() => {
            isInitialLoad = false;
        }, 1000);

        return () => {
            unsubBlink();
            unsubSystem();
            unsubNav();
            unsubSelect();
        };
    }, [addEventLog, options, playSound, speakSystemStatus, triggerBlinkPulse]);

    // Notifications handler from ESP32 BLE
    const handleNotifications = useCallback((event: Event) => {
        const target = event.target as BluetoothRemoteGATTCharacteristic;
        const value = target.value;
        if (!value) return;

        // Convert DataView to Uint8Array
        const data = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);

        // Single-byte packet: System ON (0) or System OFF (127)
        if (data.length === 1) {
            if (data[0] === 0) {
                // Menu activated
                setMenuActive(true);
                setCurrentMenuIndex(1);
                setActiveSelection(null);
                speakSystemStatus('System activated');
                writeAck('x', 1);
                syncSystemState(true);
                addEventLog('System Activated', 'SYSTEM', 'Communication mode ON (4 blinks)');
            } else if (data[0] === 127) {
                // Menu deactivated
                setMenuActive(false);
                setCurrentMenuIndex(0);
                setActiveSelection(null);
                setSelectedOption(null);
                speakSystemStatus('System inactive');
                writeAck('x', 0);
                syncSystemState(false);
                setLiveBlinkCount(0);
                addEventLog('System Inactive', 'SYSTEM', 'Communication mode OFF');
            }
        } else if (data.length === 2) {
            const cmdChar = String.fromCharCode(data[0]);
            const val = data[1];

            // 'B' = Real-time individual blink detected
            if (cmdChar === 'B') {
                triggerBlinkPulse(val);
                syncBlinkDetection(val, 'DETECTED');
                addEventLog(`Blink Detected: ${val}`, 'BLINK', `Sequential blink #${val}`);
            }
            // 'C' = Sequence completed with count
            else if (cmdChar === 'C') {
                triggerBlinkPulse(val);
                syncBlinkDetection(val, 'SEQUENCE_COMPLETED');
                addEventLog(`Sequence Finished: ${val}`, 'BLINK', `${val} total blinks executed`);
            }
            // 'R' = Sequence reset
            else if (cmdChar === 'R') {
                setLiveBlinkCount(0);
                syncBlinkDetection(0, 'RESET');
            }
            // 'S' = Menu navigation (1 blink)
            else if (cmdChar === 'S') {
                const newIndex = val;
                setMenuActive(true);
                setCurrentMenuIndex(newIndex);
                setActiveSelection(null);
                writeAck('s', newIndex);

                const item = options[newIndex - 1];
                if (item) {
                    syncCurrentNavigation(newIndex, item.id, item.label);
                    addEventLog(`Navigated to ${item.label}`, 'NAVIGATE', `Menu slot #${newIndex}`);
                }
            }
            // 'A' = Option Activated / Spoken (2 blinks)
            else if (cmdChar === 'A') {
                const selectedIndex = val;
                if (selectedIndex > 0 && selectedIndex <= options.length) {
                    const opt = options[selectedIndex - 1];
                    setSelectedOption(opt.id);
                    setActiveSelection(opt.id);
                    setLastSelectedOutput({
                        id: opt.id,
                        label: opt.label,
                        time: new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
                    });
                    playSound(opt.soundFile);
                    syncSelectedOutput(opt.id, opt.label, selectedIndex);
                    addEventLog(`Selected Output: ${opt.label}`, 'SELECT', `Triggered via 2 blinks`);

                    setTimeout(() => {
                        setActiveSelection(null);
                    }, 3000);
                    writeAck('a', selectedIndex);
                }
            }
        }
    }, [options, playSound, speakSystemStatus, writeAck, triggerBlinkPulse, addEventLog]);

    // Handle menu index changes for visual highlight and sound
    useEffect(() => {
        if (menuActive && currentMenuIndex > 0 && currentMenuIndex <= options.length) {
            const opt = options[currentMenuIndex - 1];
            setSelectedOption(opt.id);
            playSound("select.mp3");
        } else if (!menuActive) {
            setSelectedOption(null);
        }
    }, [currentMenuIndex, menuActive, options, playSound]);

    const [isConnecting, setIsConnecting] = useState(false);
    const [connectionError, setConnectionError] = useState<string | null>(null);
    const connectedDeviceRef = useRef<BluetoothDevice | null>(null);

    const handleDisconnection = useCallback(() => {
        console.log('Device disconnected');
        setIsConnected(false);
        setMenuActive(false);
        setCurrentMenuIndex(0);
        characteristicRef.current = null;
        syncConnectionState(false);
        addEventLog('Sensor Disconnected', 'CONNECT', 'BLE link closed');
    }, [addEventLog]);

    const connectToDevice = useCallback(async () => {
        try {
            setIsConnecting(true);
            setConnectionError(null);

            if (!navigator.bluetooth || !navigator.bluetooth.requestDevice) {
                throw new Error('Web Bluetooth not supported in this browser. Please use Chrome/Edge.');
            }

            const bleDevice = await navigator.bluetooth.requestDevice({
                filters: [
                    { services: ['6910123a-eb0d-4c35-9a60-bebe1dcb549d'] },
                    { name: 'ESP32C6_EEG' },
                    { namePrefix: 'ESP32' }
                ],
                optionalServices: ['6910123a-eb0d-4c35-9a60-bebe1dcb549d']
            }) as BluetoothDevice;

            connectedDeviceRef.current = bleDevice;

            if (!bleDevice.gatt) {
                throw new Error('Bluetooth device does not support GATT');
            }

            bleDevice.addEventListener('gattserverdisconnected', handleDisconnection);

            console.log('Connecting to GATT Server...');
            const server = await Promise.race([
                bleDevice.gatt.connect(),
                new Promise<never>((_, reject) =>
                    setTimeout(() => reject(new Error('Connection timed out. Ensure ESP32 is powered on.')), 10000)
                )
            ]);

            console.log('Getting Service...');
            const service = await server.getPrimaryService('6910123a-eb0d-4c35-9a60-bebe1dcb549d');

            console.log('Getting Characteristic...');
            const characteristic = await service.getCharacteristic('5f4f1107-7fc1-43b2-a540-0aa1a9f1ce78');

            await characteristic.startNotifications();
            characteristic.addEventListener('characteristicvaluechanged', handleNotifications);
            characteristicRef.current = characteristic;

            setDevice(bleDevice);
            setIsConnected(true);
            setIsConnecting(false);
            syncConnectionState(true);
            addEventLog('Sensor Connected', 'CONNECT', 'ESP32 EEG BLE stream active');

            return true;
        } catch (error) {
            console.error('Connection failed:', error);
            setIsConnecting(false);
            setIsConnected(false);
            setConnectionError(error instanceof Error ? error.message : 'Connection failed');
            return false;
        }
    }, [handleNotifications, handleDisconnection, addEventLog]);

    const disconnectDevice = useCallback(async () => {
        try {
            if (!connectedDeviceRef.current) return;
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
            syncConnectionState(false);
        }
    }, [handleNotifications]);

    const toggleConnection = async () => {
        if (isConnected) {
            await disconnectDevice();
        } else {
            await connectToDevice();
        }
    };

    const handleOptionClick = (option: Option, index: number) => {
        setSelectedOption(selectedOption === option.id ? null : option.id);
        setActiveSelection(option.id);
        setLastSelectedOutput({
            id: option.id,
            label: option.label,
            time: new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })
        });
        playSound(option.soundFile);
        
        // Push realtime sync to Firebase on click test as well
        syncSelectedOutput(option.id, option.label, index + 1);
        addEventLog(`Manual Select: ${option.label}`, 'SELECT', `UI interaction`);

        setTimeout(() => setActiveSelection(null), 2500);
    };

    const toggleTheme = () => {
        setIsDarkMode(!isDarkMode);
    };

    // Human-designed aesthetics using Tailwind utilities
    const themeClasses = {
        background: isDarkMode ? 'bg-slate-950' : 'bg-[#f8faff]',
        text: isDarkMode ? 'text-slate-100' : 'text-slate-900',
        cardBg: isDarkMode
            ? 'bg-slate-900/60 backdrop-blur-xl border border-slate-800 shadow-xl shadow-black/40'
            : 'bg-white border border-slate-200/70 shadow-[0_8px_30px_rgb(0,0,0,0.04)]',
        cardHover: isDarkMode
            ? 'hover:bg-slate-800/70 hover:border-purple-500/40 hover:-translate-y-1 hover:shadow-purple-500/10'
            : 'hover:shadow-[0_12px_35px_rgb(0,0,0,0.08)] hover:-translate-y-1 hover:border-purple-200',
        textSecondary: isDarkMode ? 'text-slate-300' : 'text-slate-600',
        textMuted: isDarkMode ? 'text-slate-400' : 'text-slate-500',
        glassBg: isDarkMode
            ? 'bg-slate-900/70 backdrop-blur-2xl border border-slate-800'
            : 'bg-white/85 backdrop-blur-xl border border-slate-200/80 shadow-[0_4px_20px_rgb(0,0,0,0.03)]'
    };

    return (
        <div className={`min-h-screen min-w-full flex flex-col transition-colors duration-500 ${themeClasses.background} ${themeClasses.text} relative overflow-x-hidden font-sans`}>
            {/* Subtle background glow */}
            <div className="absolute inset-0 overflow-hidden pointer-events-none">
                <div className={`absolute top-0 left-0 w-full h-full transition-opacity duration-1000 ${isDarkMode ? 'opacity-100' : 'opacity-0'}`}>
                    <div className="absolute top-[-10%] left-[-5%] w-[45%] h-[45%] bg-purple-900/25 blur-[140px] rounded-full"></div>
                    <div className="absolute top-[30%] right-[-10%] w-[40%] h-[40%] bg-blue-900/20 blur-[140px] rounded-full"></div>
                    <div className="absolute bottom-[-10%] left-[20%] w-[50%] h-[35%] bg-indigo-900/20 blur-[140px] rounded-full"></div>
                </div>
                <div className={`absolute top-0 left-0 w-full h-full transition-opacity duration-1000 ${isDarkMode ? 'opacity-0' : 'opacity-100'}`}>
                    <div className="absolute top-0 right-0 w-[70%] h-[50%] bg-gradient-to-bl from-blue-100/40 via-purple-50/40 to-transparent blur-3xl"></div>
                    <div className="absolute bottom-0 left-0 w-[60%] h-[60%] bg-gradient-to-tr from-purple-100/40 via-sky-50/30 to-transparent blur-3xl"></div>
                </div>
            </div>

            <div className="flex-1 flex flex-col px-4 py-6 sm:px-6 sm:py-8 md:px-10 max-w-7xl mx-auto w-full relative z-10">

                {/* Top Header */}
                <div className="flex flex-col sm:flex-row sm:justify-between sm:items-center mb-6 gap-5">
                    <div className="flex items-center space-x-3">
                        <div className="w-11 h-11 rounded-2xl bg-gradient-to-br from-purple-600 to-indigo-600 flex items-center justify-center text-white shadow-lg shadow-purple-500/25">
                            <Eye size={24} className="animate-pulse" />
                        </div>
                        <div>
                            <h1 className={`text-2xl sm:text-3xl md:text-4xl font-extrabold tracking-tight ${isDarkMode ? 'text-white' : 'text-slate-900'}`}>
                                Neuro<span className="text-purple-600 dark:text-purple-400">Speak</span>
                            </h1>
                            <p className={`text-xs sm:text-sm font-medium ${themeClasses.textMuted}`}>
                                EEG Real-Time Assistive Communication
                            </p>
                        </div>
                    </div>

                    {/* Actions & Connection controls */}
                    <div className="flex flex-wrap items-center gap-3">
                        <button
                            onClick={toggleTheme}
                            className={`p-2.5 rounded-xl flex items-center justify-center ${themeClasses.glassBg} hover:scale-105 active:scale-95 transition-all text-slate-600 dark:text-slate-300`}
                            aria-label="Toggle theme"
                        >
                            {isDarkMode ? <Sun size={18} /> : <Moon size={18} />}
                        </button>

                        <div className={`flex items-center space-x-2.5 px-3.5 py-2.5 rounded-xl ${themeClasses.glassBg}`}>
                            <div className={`relative flex items-center justify-center w-2.5 h-2.5 rounded-full ${isConnected ? 'bg-emerald-500' : isConnecting ? 'bg-amber-500 animate-pulse' : 'bg-slate-400'}`}>
                                {isConnected && <div className="absolute w-full h-full rounded-full bg-emerald-500 animate-ping opacity-40"></div>}
                            </div>
                            <span className="text-xs sm:text-sm font-semibold">
                                {isConnected ? 'Sensor Connected' : isConnecting ? 'Connecting...' : 'Sensor Ready'}
                            </span>
                        </div>

                        <button
                            onClick={toggleConnection}
                            disabled={isConnecting}
                            className={`px-5 py-2.5 rounded-xl font-semibold transition-all text-xs sm:text-sm flex items-center space-x-2
                                ${isConnected
                                    ? 'bg-slate-200 text-slate-800 hover:bg-slate-300 dark:bg-slate-800 dark:text-slate-200 dark:hover:bg-slate-700'
                                    : 'bg-gradient-to-r from-purple-600 to-indigo-600 hover:from-purple-700 hover:to-indigo-700 text-white shadow-lg shadow-purple-500/25'
                                }
                                ${isConnecting ? 'opacity-75 cursor-wait' : 'hover:-translate-y-0.5'}
                            `}
                        >
                            <Radio size={16} className={isConnected ? "animate-pulse text-emerald-400" : ""} />
                            <span>{isConnecting ? 'Connecting...' : isConnected ? 'Disconnect' : 'Connect Bluetooth'}</span>
                        </button>
                    </div>
                </div>

                {connectionError && (
                    <div className="mb-5 p-3.5 rounded-xl bg-red-500/10 border border-red-500/30 text-red-600 dark:text-red-400 text-xs sm:text-sm flex items-center space-x-2">
                        <span className="font-semibold">Error:</span>
                        <span>{connectionError}</span>
                    </div>
                )}

                {/* FIREBASE REALTIME TELEMETRY PANEL */}
                <div className="mb-6 grid grid-cols-1 md:grid-cols-4 gap-3 sm:gap-4">
                    {/* Realtime Blink Count Card */}
                    <div className={`rounded-2xl p-4 transition-all duration-300 border ${
                        blinkAnimTrigger
                            ? (isDarkMode ? 'bg-purple-900/40 border-purple-400 shadow-lg shadow-purple-500/20 scale-[1.02]' : 'bg-purple-100 border-purple-400 shadow-md scale-[1.02]')
                            : themeClasses.cardBg
                    }`}>
                        <div className="flex items-center justify-between mb-2">
                            <span className={`text-xs font-bold uppercase tracking-wider ${themeClasses.textMuted} flex items-center gap-1.5`}>
                                <Activity size={14} className="text-purple-500" /> Live Blink Count
                            </span>
                            <span className="text-[11px] px-2 py-0.5 rounded-full bg-purple-500/10 text-purple-600 dark:text-purple-300 font-semibold">
                                Realtime
                            </span>
                        </div>
                        <div className="flex items-baseline space-x-3">
                            <span className={`text-3xl sm:text-4xl font-extrabold transition-all duration-200 ${blinkAnimTrigger ? 'text-purple-600 dark:text-purple-300 scale-110' : ''}`}>
                                {liveBlinkCount}
                            </span>
                            <span className={`text-xs ${themeClasses.textMuted}`}>
                                {liveBlinkCount === 0 ? 'Awaiting blinks' : liveBlinkCount === 1 ? 'Next' : liveBlinkCount === 2 ? 'Speak' : liveBlinkCount === 4 ? 'Toggle On/Off' : 'Counting...'}
                            </span>
                        </div>
                        <div className="mt-2.5 w-full bg-slate-200 dark:bg-slate-800 rounded-full h-1.5 overflow-hidden">
                            <div 
                                className="bg-gradient-to-r from-purple-500 to-indigo-500 h-1.5 rounded-full transition-all duration-300"
                                style={{ width: `${Math.min(liveBlinkCount * 25, 100)}%` }}
                            ></div>
                        </div>
                    </div>

                    {/* Selected Output Card */}
                    <div className={`rounded-2xl p-4 border transition-all duration-300 ${
                        activeSelection
                            ? (isDarkMode ? 'bg-emerald-950/40 border-emerald-400 shadow-lg shadow-emerald-500/20' : 'bg-emerald-50 border-emerald-400 shadow-md')
                            : themeClasses.cardBg
                    }`}>
                        <div className="flex items-center justify-between mb-2">
                            <span className={`text-xs font-bold uppercase tracking-wider ${themeClasses.textMuted} flex items-center gap-1.5`}>
                                <Sparkles size={14} className="text-emerald-500" /> Selected Output
                            </span>
                            {lastSelectedOutput && (
                                <span className="text-[10px] px-1.5 py-0.5 rounded bg-emerald-500/10 text-emerald-600 dark:text-emerald-300 font-medium">
                                    {lastSelectedOutput.time}
                                </span>
                            )}
                        </div>
                        <div className="flex items-center space-x-2">
                            <div className="text-2xl sm:text-3xl font-extrabold truncate">
                                {lastSelectedOutput ? (
                                    <span className="text-emerald-600 dark:text-emerald-400">{lastSelectedOutput.label}</span>
                                ) : (
                                    <span className={`text-sm font-medium ${themeClasses.textMuted}`}>None yet</span>
                                )}
                            </div>
                        </div>
                        <p className={`text-xs mt-1 truncate ${themeClasses.textMuted}`}>
                            {activeSelection ? 'Active audio synthesized' : 'Triggered via 2 consecutive blinks'}
                        </p>
                    </div>

                    {/* System Activated State Card */}
                    <div className={`rounded-2xl p-4 border transition-all duration-300 ${
                        menuActive
                            ? (isDarkMode ? 'bg-indigo-950/40 border-indigo-400 shadow-lg shadow-indigo-500/20' : 'bg-indigo-50 border-indigo-400 shadow-md')
                            : themeClasses.cardBg
                    }`}>
                        <div className="flex items-center justify-between mb-2">
                            <span className={`text-xs font-bold uppercase tracking-wider ${themeClasses.textMuted} flex items-center gap-1.5`}>
                                <Radio size={14} className="text-indigo-500" /> System State
                            </span>
                            <div className={`w-2.5 h-2.5 rounded-full ${menuActive ? 'bg-emerald-500 animate-pulse' : 'bg-slate-400'}`}></div>
                        </div>
                        <div className="flex items-baseline space-x-2">
                            <span className={`text-2xl sm:text-3xl font-extrabold ${menuActive ? 'text-indigo-600 dark:text-indigo-400' : themeClasses.textMuted}`}>
                                {menuActive ? 'ACTIVATED' : 'INACTIVE'}
                            </span>
                        </div>
                        <p className={`text-xs mt-1 ${themeClasses.textMuted}`}>
                            {menuActive ? 'Communication Mode Active' : 'Blink 4 times to activate'}
                        </p>
                    </div>

                    {/* Firebase Cloud Sync Status Card */}
                    <div className={`rounded-2xl p-4 border ${themeClasses.cardBg}`}>
                        <div className="flex items-center justify-between mb-2">
                            <span className={`text-xs font-bold uppercase tracking-wider ${themeClasses.textMuted} flex items-center gap-1.5`}>
                                <Flame size={14} className="text-amber-500" /> Firebase RTDB
                            </span>
                            <span className="flex items-center gap-1 text-[11px] px-2 py-0.5 rounded-full bg-amber-500/10 text-amber-600 dark:text-amber-300 font-semibold">
                                <span className="w-1.5 h-1.5 rounded-full bg-amber-500 animate-ping"></span> Live
                            </span>
                        </div>
                        <div className="flex items-center space-x-2">
                            <Database size={20} className="text-amber-500" />
                            <span className="text-sm sm:text-base font-bold truncate">
                                neurospeak2
                            </span>
                        </div>
                        <p className={`text-[11px] mt-1.5 truncate font-mono ${themeClasses.textMuted}`}>
                            /neurospeak/realtime
                        </p>
                    </div>
                </div>

                {/* Communication Mode Active Banner */}
                <div className={`transition-all duration-500 ease-in-out overflow-hidden ${menuActive ? 'max-h-24 opacity-100 mb-6' : 'max-h-0 opacity-0 mb-0'}`}>
                    <div className={`relative overflow-hidden border rounded-2xl p-4 flex items-center justify-between shadow-lg 
                        ${isDarkMode ? 'bg-purple-900/30 border-purple-500/30 shadow-purple-900/20' : 'bg-purple-50/90 border-purple-200 shadow-purple-500/10'}`}>
                        <div className="flex items-center space-x-3">
                            <div className={`w-3.5 h-3.5 rounded-full animate-pulse ${isDarkMode ? 'bg-purple-400 shadow-[0_0_12px_rgba(192,132,252,0.9)]' : 'bg-purple-600 shadow-[0_0_10px_rgba(168,85,247,0.7)]'}`}></div>
                            <div>
                                <h3 className={`font-bold text-base sm:text-lg tracking-wide ${isDarkMode ? 'text-purple-300' : 'text-purple-800'}`}>
                                    Communication Mode Active
                                </h3>
                                <p className={`text-xs ${isDarkMode ? 'text-purple-400/80' : 'text-purple-600'}`}>
                                    1 Blink: Next Option • 2 Blinks: Speak & Select • 4 Blinks: Exit
                                </p>
                            </div>
                        </div>
                        <span className={`text-xs font-semibold px-3 py-1 rounded-full ${isDarkMode ? 'bg-purple-500/20 text-purple-300' : 'bg-purple-200/70 text-purple-800'}`}>
                            Slot {currentMenuIndex > 0 ? currentMenuIndex : 1} of {options.length}
                        </span>
                    </div>
                </div>

                {/* Communication Grid */}
                <div className="w-full mb-8">
                    <div className="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-6 gap-3 sm:gap-5">
                        {options.map((option, index) => {
                            const isSelected = selectedOption === option.id;
                            const isCurrentMenuOption = menuActive && currentMenuIndex === index + 1;
                            const isActiveSelection = activeSelection === option.id;

                            return (
                                <div
                                    key={option.id}
                                    onClick={() => handleOptionClick(option, index)}
                                    className={`
                                        aspect-[4/3] lg:aspect-square relative group cursor-pointer transition-all duration-300
                                        ${themeClasses.cardBg} ${themeClasses.cardHover}
                                        rounded-2xl p-4 sm:p-5 border-2 flex flex-col items-center justify-center select-none
                                        ${isSelected ? (isDarkMode ? 'border-purple-500 bg-purple-500/15 shadow-lg shadow-purple-500/20' : 'border-purple-500 bg-purple-50 shadow-md') : 'border-transparent'}
                                        ${isCurrentMenuOption ? (isDarkMode ? 'ring-4 ring-purple-400/60 ring-offset-2 ring-offset-slate-950 scale-105' : 'ring-4 ring-purple-500/50 ring-offset-2 ring-offset-white scale-105') : ''}
                                        ${isActiveSelection ? 'scale-110 shadow-2xl ring-4 ring-emerald-500' : ''}
                                    `}
                                >
                                    {/* Number badge */}
                                    <div className="absolute top-2.5 left-2.5 text-[11px] font-bold px-2 py-0.5 rounded-md bg-slate-500/10 text-slate-500">
                                        #{index + 1}
                                    </div>

                                    <div className={`
                                        p-3 sm:p-4 rounded-2xl transition-transform duration-500 group-hover:scale-110 mb-3
                                        ${isDarkMode ? 'bg-white/5' : option.lightColor.split(' ')[0]}
                                        ${isSelected ? (isDarkMode ? 'bg-purple-500/20 text-purple-300' : 'text-purple-600') : (isDarkMode ? 'text-slate-300' : option.lightColor.split(' ')[1])}
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

                {/* Bottom Section: Gestures Guide + Realtime Cloud Activity Log */}
                <div className="mt-auto grid grid-cols-1 lg:grid-cols-3 gap-6">
                    {/* NeuroSpeak Gestures */}
                    <div className="lg:col-span-2">
                        <h3 className={`text-base sm:text-lg font-bold mb-3 ${themeClasses.textSecondary} flex items-center gap-2`}>
                            <Activity size={18} className="text-purple-500" /> NeuroSpeak Blink Gestures
                        </h3>
                        <div className="grid grid-cols-1 sm:grid-cols-2 gap-3">
                            {[
                                { step: '01', title: 'Activate', desc: '4 blinks toggles Communication Mode ON.', color: 'bg-blue-500', lightColor: 'bg-blue-50 border-blue-100 text-blue-600' },
                                { step: '02', title: 'Navigate', desc: '1 blink advances to the next menu item.', color: 'bg-purple-500', lightColor: 'bg-purple-50 border-purple-100 text-purple-600' },
                                { step: '03', title: 'Select & Speak', desc: '2 blinks speaks and selects highlighted item.', color: 'bg-pink-500', lightColor: 'bg-pink-50 border-pink-100 text-pink-600' },
                                { step: '04', title: 'Deactivate', desc: '4 blinks or 20s idle timeout to deactivate.', color: 'bg-emerald-500', lightColor: 'bg-emerald-50 border-emerald-100 text-emerald-600' }
                            ].map((item, index) => (
                                <div
                                    key={index}
                                    className={`${themeClasses.glassBg} p-3.5 rounded-2xl flex flex-col justify-start transition-all hover:scale-[1.01]`}
                                >
                                    <div className="flex items-center space-x-3 mb-1.5">
                                        <div className={`w-7 h-7 rounded-lg flex items-center justify-center font-bold text-xs
                                            ${isDarkMode ? `${item.color} text-white shadow` : `${item.lightColor} border`}
                                        `}>
                                            {item.step}
                                        </div>
                                        <div className="font-bold text-sm sm:text-base">{item.title}</div>
                                    </div>
                                    <div className={`text-xs ${themeClasses.textMuted} leading-relaxed`}>{item.desc}</div>
                                </div>
                            ))}
                        </div>
                    </div>

                    {/* Live Realtime Cloud Activity Stream */}
                    <div className={`${themeClasses.glassBg} rounded-2xl p-4 flex flex-col`}>
                        <div className="flex items-center justify-between mb-3">
                            <h3 className={`text-sm font-bold ${themeClasses.textSecondary} flex items-center gap-1.5`}>
                                <Flame size={16} className="text-amber-500" /> Live Firebase Stream
                            </h3>
                            <span className="text-[10px] text-slate-400 font-mono">RTDB synced</span>
                        </div>

                        <div className="space-y-2 flex-1 overflow-y-auto max-h-48 text-xs font-mono">
                            {recentEvents.length === 0 ? (
                                <div className={`text-xs italic ${themeClasses.textMuted} py-6 text-center`}>
                                    Awaiting real-time events...
                                </div>
                            ) : (
                                recentEvents.map((evt) => (
                                    <div key={evt.id} className="p-2 rounded-lg bg-slate-500/5 border border-slate-500/10 flex items-start justify-between gap-2">
                                        <div className="truncate">
                                            <div className="font-bold text-[11px] truncate flex items-center gap-1">
                                                <span className={`w-1.5 h-1.5 rounded-full ${
                                                    evt.type === 'BLINK' ? 'bg-purple-500' :
                                                    evt.type === 'SELECT' ? 'bg-emerald-500' :
                                                    evt.type === 'SYSTEM' ? 'bg-blue-500' :
                                                    'bg-slate-400'
                                                }`}></span>
                                                {evt.title}
                                            </div>
                                            <div className={`text-[10px] truncate ${themeClasses.textMuted}`}>{evt.detail}</div>
                                        </div>
                                        <span className="text-[10px] text-slate-400 whitespace-nowrap">{evt.time}</span>
                                    </div>
                                ))
                            )}
                        </div>
                    </div>
                </div>

            </div>
        </div>
    );
};

export default CommunicationInterface;