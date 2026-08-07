# 👁️✨ Eye Blink System (Mind-to-World / M2W) 🚀

Hello little friend! 🖐️ Welcome to the **Eye Blink System**! 

Imagine you have **magic eyes**! 👀 When you blink, a computer screen talks back to you and helps you ask for **Water 💧**, **Food 🍕**, **Call Nurse 👩‍⚕️**, or turn on the **Lights 💡** — without using your hands at all! Isn't that super cool? 

---

## 🧙‍♂️ How Does The Magic Work? (In 3 Simple Steps!)

1. 👁️ **You Blink Your Eye!** (Quick blink = *Move next*, Long blink = *Choose!*)
2. 🤖 **The Little Robot Hardware (Firmware)** senses your blink, cleans up the signal, and sends an invisible Bluetooth message through the air! 📶
3. 📺 **The Magic Screen (Web Dashboard)** hears the message, highlights the button, and speaks out loud: *"Water please!"* 🗣️

---

## 🧰 What's Inside The Magic Toy Box? (Project Structure)

Our project has two main super-hero teams:

```text
Eye-Blink-System/
├── 🤖 firmware/    <-- The Sensor Brain (Hardware Code for ESP32 Chip)
└── 💻 M2W-main/     <-- The Magic Screen (Web App on Next.js & React)
```

---

## 🤖 Team 1: The Sensor Brain (`/firmware`)

This team lives inside a tiny chip near your glasses/eyes. It listens to your eyes and talks over Bluetooth.

| 📄 File Name | 🧸 Nursery Kid Explanation |
| :--- | :--- |
| **`firmware.ino`** 👑 | **The Big Robot Captain!** <br> Runs the whole hardware show! It reads sensor signals, talks to Bluetooth, and coordinates all the helpers below. |
| **`BlinkDetector.h` & `.cpp`** 👁️⚡ | **The Blink Counter!** <br> Watches your eyes closely. It asks: *"Was that a short blink or did you hold your eye shut tight?"* |
| **`AdaptiveThreshold.h` & `.cpp`** 📏 | **The Smart Sensitivity Ruler!** <br> Everyone blinks differently! This ruler automatically learns how hard or soft you blink so it doesn't get confused. |
| **`Filters.h` & `.cpp`** 🧹 | **The Dust Sweeper!** <br> Sweeps away noisy wiggles and electrical static, leaving only clean, smooth blink signals. |
| **`SequenceClassifier.h` & `.cpp`** 🧩 | **The Blink Decoder Puzzle!** <br> Turns blink patterns (like double-blink) into action commands like *NEXT*, *SELECT*, or *EMERGENCY*! |
| **`Calibration.h` & `.cpp`** 🎓 | **The School Teacher!** <br> Teaches the robot brain about your eye when you first turn the device on. |
| **`StateMachine.h` & `.cpp`** 🚦 | **The Traffic Light!** <br> Remembers what mode the device is in: *Sleeping*, *Learning (Calibrating)*, or *Ready to Work*! |
| **`partitions.csv`** 🗄️ | **The Memory Locker Map!** <br> Shows the tiny chip where to store its code and memory drawers. |

---

## 💻 Team 2: The Magic Web Screen (`/M2W-main`)

This team shows a beautiful screen on your computer or tablet, connects via Bluetooth, and speaks out loud!

| 📄 File Name | 🧸 Nursery Kid Explanation |
| :--- | :--- |
| **`src/components/Mainpage.tsx`** 📺✨ | **The Super Magic Interactive Screen!** <br> This is the star of the website! It connects to Bluetooth, draws big colorful menu cards (Nurse, Water, Food, Emergency), moves the highlight when you blink, and talks out loud using voice! |
| **`src/app/page.tsx`** 🚪 | **The Front Door!** <br> The first door you open when you visit the website. It immediately opens the main magic screen. |
| **`src/app/layout.tsx`** 🏠 | **The House Frame!** <br> Keeps the webpage neat and structured with nice fonts and titles. |
| **`src/app/globals.css`** 🎨 | **The Color Paint Box!** <br> Gives the website pretty dark modes, neon glows, smooth animations, and gorgeous colors. |
| **`package.json`** 📜 | **The Recipe & Ingredients List!** <br> Lists all the helper libraries (React, Next.js, Lucide icons, Tailwind) needed to bake this website! |
| **`next.config.ts`** ⚙️ | **The Control Knob!** <br> Tells Next.js how to run smoothly. |

---

## 🏃 Quick Start: How to Play with It!

### 1. Launch the Magic Web Screen
Open terminal in `M2W-main` folder and run:
```bash
npm run dev
```
Then open [http://localhost:3000](http://localhost:3000) in your browser!

### 2. Connect Your Eye Sensor
Click **"Connect Device"** on the screen, select your Eye-Blink Bluetooth device, and start blinking to control the world! 🌟

---

*Made with ❤️ for making the world accessible to everyone through magic blinks!*
