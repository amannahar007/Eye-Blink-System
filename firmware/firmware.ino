// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE See the
// GNU General Public License for more details.

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "esp_dsp.h"
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ============================================================
// WIFI & FIREBASE CONFIGURATION (HOTSPOT)
// ============================================================

const char* WIFI_SSID     = "Neuro";
const char* WIFI_PASS     = "123@#$456";
const char* FIREBASE_URL  = "https://neurospeak2-default-rtdb.firebaseio.com";
const char* FIREBASE_AUTH = "AIzaSyCIWR_XGD-UGltZge3hIoDmqtGavsXLFcs";

// Menu option names matching M2W Web App
const char* OPTION_IDS[]    = { "food", "help", "outing", "television", "washroom", "water" };
const char* OPTION_LABELS[] = { "Food", "Help", "Outing", "Television", "Washroom", "Water" };

enum FirebaseEventType {
  FB_EVT_BLINK,
  FB_EVT_SYSTEM_STATE,
  FB_EVT_NAVIGATION,
  FB_EVT_SELECTION
};

struct FirebaseEvent {
  FirebaseEventType type;
  int value;
  int extra;
};

QueueHandle_t firebaseQueue = nullptr;

// ============================================================
// USER CONFIGURATION
// ============================================================

#define SAMPLE_RATE       512
#define BAUD_RATE         115200
#define INPUT_PIN         A0
#define PIXEL_PIN         15
#define PIXEL_COUNT       6

#define BLINK_SERVICE_UUID \
  "6910123a-eb0d-4c35-9a60-bebe1dcb549d"

#define BLINK_CHAR_UUID \
  "5f4f1107-7fc1-43b2-a540-0aa1a9f1ce78"

// ============================================================
// BLINK SETTINGS
// ============================================================

// Minimum time between two detected blinks
const unsigned long BLINK_DEBOUNCE_MS = 250;

// Maximum time allowed between blinks in one sequence
// Example:
// 1 blink
// 1st + 2nd blink
// 1st + 2nd + 3rd blink
// 1st + 2nd + 3rd + 4th blink
const unsigned long BLINK_SEQUENCE_TIMEOUT_MS = 900;

// EEG envelope threshold
float BlinkThreshold = 50.0;

// ============================================================
// ENVELOPE SETTINGS
// ============================================================

#define ENVELOPE_WINDOW_MS   100
#define ENVELOPE_WINDOW_SIZE ((ENVELOPE_WINDOW_MS * SAMPLE_RATE) / 1000)

// ============================================================
// BLE
// ============================================================

BLEServer* pBleServer = nullptr;
BLEService* pBlinkService = nullptr;
BLECharacteristic* pBlinkChar = nullptr;

bool clientConnected = false;

// ============================================================
// NEOPIXEL
// ============================================================

Adafruit_NeoPixel pixels(
  PIXEL_COUNT,
  PIXEL_PIN,
  NEO_GRB + NEO_KHZ800
);

// ============================================================
// SYSTEM / MENU STATE
// ============================================================

// menu == true  -> system ON
// menu == false -> system OFF

bool menu = false;

int menuIndex = 0;

const unsigned long MENU_TIMEOUT_MS = 20000;
unsigned long menuStartTime = 0;

// ============================================================
// BLINK STATE MACHINE
// ============================================================

int pendingBlinkCount = 0;

unsigned long firstBlinkTime = 0;
unsigned long lastDetectedBlinkTime = 0;

// Prevent one long threshold crossing from becoming
// multiple blinks.
bool thresholdWasHigh = false;

// ============================================================
// EEG ENVELOPE BUFFER
// ============================================================

float envelopeBuffer[ENVELOPE_WINDOW_SIZE] = {0};
int envelopeIndex = 0;
float envelopeSum = 0;
float currentEEGEnvelope = 0;

// ============================================================
// BANDPOWER STRUCTURE
// ============================================================

typedef struct {
  float delta;
  float theta;
  float alpha;
  float beta;
  float gamma;
  float total;
} BandpowerResults;

BandpowerResults smoothedPowers = {0};

// ============================================================
// BLE CALLBACKS
// ============================================================

class MyServerCallbacks : public BLEServerCallbacks {

  void onConnect(BLEServer* pServer) override {

    clientConnected = true;

    Serial.println("BLE client connected");
  }

  void onDisconnect(BLEServer* pServer) override {

    clientConnected = false;

    Serial.println("BLE client disconnected");

    // Restart advertising
    pServer->getAdvertising()->start();
  }
};

// ============================================================
// HIGH PASS FILTER
// ============================================================

float highpass(float input)
{
  float output = input;

  {
    static float z1, z2;

    float x =
      output
      - -1.91327599 * z1
      - 0.91688335 * z2;

    output =
      0.95753983 * x
      + -1.91507967 * z1
      + 0.95753983 * z2;

    z2 = z1;
    z1 = x;
  }

  return output;
}

// ============================================================
// NOTCH FILTER
// ============================================================

float Notch(float input)
{
  float output = input;

  {
    static float z1, z2;

    float x =
      output
      - -1.58696045 * z1
      - 0.96505858 * z2;

    output =
      0.96588529 * x
      + -1.57986211 * z1
      + 0.96588529 * z2;

    z2 = z1;
    z1 = x;
  }

  {
    static float z1, z2;

    float x =
      output
      - -1.62761184 * z1
      - 0.96671306 * z2;

    output =
      1.00000000 * x
      + -1.63566226 * z1
      + 1.00000000 * z2;

    z2 = z1;
    z1 = x;
  }

  return output;
}

// ============================================================
// LOW PASS FILTER
// ============================================================

float EEGFilter(float input)
{
  float output = input;

  {
    static float z1, z2;

    float x =
      output
      - -1.24200128 * z1
      - 0.45885207 * z2;

    output =
      0.05421270 * x
      + 0.10842539 * z1
      + 0.05421270 * z2;

    z2 = z1;
    z1 = x;
  }

  return output;
}

// ============================================================
// EEG ENVELOPE
// ============================================================

float updateEEGEnvelope(float sample)
{
  float absSample = fabs(sample);

  envelopeSum -= envelopeBuffer[envelopeIndex];

  envelopeSum += absSample;

  envelopeBuffer[envelopeIndex] = absSample;

  envelopeIndex =
    (envelopeIndex + 1) % ENVELOPE_WINDOW_SIZE;

  return envelopeSum / ENVELOPE_WINDOW_SIZE;
}

// ============================================================
// PIXEL DISPLAY
// ============================================================

void showPixels()
{
  for (int i = 0; i < PIXEL_COUNT; i++) {

    pixels.setPixelColor(
      i,
      pixels.Color(20, 20, 20)
    );
  }

  pixels.show();
}

// ============================================================
// CLEAR PIXELS
// ============================================================

void clearPixels()
{
  pixels.clear();
  pixels.show();
}

// ============================================================
// FIREBASE REALTIME WORKER TASK (FreeRTOS Background Queue)
// ============================================================

void firebaseTask(void* parameter) {
  Serial.println("[Firebase RTDB] Background task started.");

  FirebaseEvent evt;
  while (true) {
    if (xQueueReceive(firebaseQueue, &evt, portMAX_DELAY) == pdTRUE) {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[Firebase RTDB] WiFi not connected. Attempting reconnection...");
        WiFi.reconnect();
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("[Firebase RTDB] WiFi reconnection failed. Check SSID / Password.");
          continue;
        }
        Serial.printf("[Firebase RTDB] WiFi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
      }

      char path[180];
      char payload[256];

      switch (evt.type) {
        case FB_EVT_BLINK:
          snprintf(path, sizeof(path), "%s/neurospeak/realtime/blink_count.json", FIREBASE_URL);
          snprintf(payload, sizeof(payload), "{\"blinkCount\":%d,\"phase\":\"%s\",\"timestamp\":%lu}", 
                   evt.value, (evt.extra == 1 ? "SEQUENCE_COMPLETED" : "DETECTED"), millis());
          break;

        case FB_EVT_SYSTEM_STATE:
          snprintf(path, sizeof(path), "%s/neurospeak/realtime/system_activated.json", FIREBASE_URL);
          snprintf(payload, sizeof(payload), "{\"activated\":%s,\"status\":\"%s\",\"timestamp\":%lu}", 
                   evt.value ? "true" : "false", evt.value ? "ACTIVE" : "INACTIVE", millis());
          break;

        case FB_EVT_NAVIGATION: {
          int idx = evt.value;
          const char* optId = (idx >= 1 && idx <= 6) ? OPTION_IDS[idx - 1] : "unknown";
          const char* optLabel = (idx >= 1 && idx <= 6) ? OPTION_LABELS[idx - 1] : "Unknown";
          snprintf(path, sizeof(path), "%s/neurospeak/realtime/current_navigation.json", FIREBASE_URL);
          snprintf(payload, sizeof(payload), "{\"index\":%d,\"optionId\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu}", 
                   idx, optId, optLabel, millis());
          break;
        }

        case FB_EVT_SELECTION: {
          int idx = evt.value;
          const char* optId = (idx >= 1 && idx <= 6) ? OPTION_IDS[idx - 1] : "unknown";
          const char* optLabel = (idx >= 1 && idx <= 6) ? OPTION_LABELS[idx - 1] : "Unknown";
          snprintf(path, sizeof(path), "%s/neurospeak/realtime/selected_output.json", FIREBASE_URL);
          snprintf(payload, sizeof(payload), "{\"index\":%d,\"optionId\":\"%s\",\"label\":\"%s\",\"timestamp\":%lu}", 
                   idx, optId, optLabel, millis());
          break;
        }
      }

      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient https;
      https.setTimeout(4000);
      https.setReuse(false);

      if (https.begin(client, path)) {
        https.addHeader("Content-Type", "application/json");
        int httpCode = https.PUT(payload);
        if (httpCode == HTTP_CODE_OK || httpCode == 200) {
          Serial.printf("[Firebase RTDB] Synced successfully (HTTP %d)\n", httpCode);
        } else if (httpCode > 0) {
          Serial.printf("[Firebase RTDB] Server returned HTTP %d. Check database rules.\n", httpCode);
        } else {
          Serial.printf("[Firebase RTDB] Connection failed (%d): %s\n", httpCode, https.errorToString(httpCode).c_str());
        }
        https.end();
      }
      client.stop();
    }
  }
}

void queueFirebaseEvent(FirebaseEventType type, int value, int extra = 0) {
  if (firebaseQueue != nullptr) {
    FirebaseEvent evt = { type, value, extra };
    xQueueSend(firebaseQueue, &evt, 0); // Non-blocking!
  }
}

// ============================================================
// SEND BLE 1 BYTE COMMAND
// ============================================================

void sendBLEByte(uint8_t command)
{
  if (!clientConnected) {
    return;
  }

  pBlinkChar->setValue(&command, 1);
  pBlinkChar->notify();
}

// ============================================================
// SEND BLE 2 BYTE COMMAND
// ============================================================

void sendBLECommand(char command, uint8_t value)
{
  if (!clientConnected) {
    return;
  }

  uint8_t data[2];

  data[0] = (uint8_t)command;
  data[1] = value;

  pBlinkChar->setValue(data, 2);
  pBlinkChar->notify();
}

// ============================================================
// SYSTEM ON
// ============================================================

void systemON()
{
  menu = true;

  menuStartTime = millis();

  // Start from first menu item
  menuIndex = 0;

  Serial.println();
  Serial.println("================================");
  Serial.println("SYSTEM ON");
  Serial.println("================================");

  // Existing protocol:
  // 0 = system/menu ON
  sendBLEByte(0);

  // Sync to Firebase directly over Wi-Fi Hotspot
  queueFirebaseEvent(FB_EVT_SYSTEM_STATE, 1);

  clearPixels();

  pendingBlinkCount = 0;
}

// ============================================================
// SYSTEM OFF
// ============================================================

void systemOFF()
{
  menu = false;

  menuIndex = 0;

  Serial.println();
  Serial.println("================================");
  Serial.println("SYSTEM OFF");
  Serial.println("================================");

  // Existing protocol:
  // 127 = system/menu OFF
  sendBLEByte(127);

  // Sync to Firebase directly over Wi-Fi Hotspot
  queueFirebaseEvent(FB_EVT_SYSTEM_STATE, 0);

  clearPixels();

  pendingBlinkCount = 0;
}

// ============================================================
// TOGGLE SYSTEM
// ============================================================

void toggleSystem()
{
  if (menu) {

    systemOFF();

  } else {

    systemON();
  }
}

// ============================================================
// NEXT / AAGE
// ============================================================

void nextItem()
{
  // If system is OFF, a normal 1-blink does nothing.
  // Only 4 blinks can turn the system ON.
  if (!menu) {

    Serial.println("1 blink ignored - SYSTEM OFF");

    return;
  }

  menuStartTime = millis();

  // Move to next item
  if (menuIndex >= PIXEL_COUNT) {

    menuIndex = 1;

  } else {

    menuIndex++;
  }

  Serial.print("NEXT -> Item ");
  Serial.println(menuIndex);

  // Existing protocol:
  // S + item number
  sendBLECommand(
    'S',
    (uint8_t)menuIndex
  );

  // Sync to Firebase directly over Wi-Fi Hotspot
  queueFirebaseEvent(FB_EVT_NAVIGATION, menuIndex);

  // Update LEDs
  showPixels();

  pixels.setPixelColor(
    menuIndex - 1,
    pixels.Color(0, 0, 20)
  );

  pixels.show();
}

// ============================================================
// SPEAK
// ============================================================

void speakCurrentItem()
{
  if (!menu) {

    Serial.println("2 blinks ignored - SYSTEM OFF");

    return;
  }

  if (menuIndex <= 0) {

    Serial.println("SPEAK ignored - no item selected");

    return;
  }

  menuStartTime = millis();

  Serial.print("SPEAK -> Item ");
  Serial.println(menuIndex);

  // Existing protocol:
  // A + item number
  sendBLECommand(
    'A',
    (uint8_t)menuIndex
  );

  // Sync to Firebase directly over Wi-Fi Hotspot
  queueFirebaseEvent(FB_EVT_SELECTION, menuIndex);
}

// ============================================================
// EXECUTE BLINK SEQUENCE
// ============================================================

void executeBlinkSequence(int count)
{
  Serial.println();
  Serial.print("Blink sequence completed: ");
  Serial.println(count);

  queueFirebaseEvent(FB_EVT_BLINK, count, 1);

  switch (count) {

    // --------------------------------------------------------
    // 1 BLINK = NEXT
    // --------------------------------------------------------

    case 1:

      Serial.println("ACTION: 1 BLINK -> NEXT");

      nextItem();

      break;

    // --------------------------------------------------------
    // 2 BLINKS = SPEAK
    // --------------------------------------------------------

    case 2:

      Serial.println("ACTION: 2 BLINKS -> SPEAK");

      speakCurrentItem();

      break;

    // --------------------------------------------------------
    // 3 BLINKS = NO ACTION
    // --------------------------------------------------------

    case 3:

      Serial.println("ACTION: 3 BLINKS -> NO ACTION");

      break;

    // --------------------------------------------------------
    // 4 BLINKS = SYSTEM ON/OFF
    // --------------------------------------------------------

    case 4:

      Serial.println("ACTION: 4 BLINKS -> SYSTEM ON/OFF");

      toggleSystem();

      break;

    // --------------------------------------------------------
    // 5+ BLINKS
    // --------------------------------------------------------

    default:

      Serial.println("ACTION: INVALID BLINK COUNT -> IGNORE");

      break;
  }
}

// ============================================================
// REGISTER ONE BLINK
// ============================================================

void registerBlink(unsigned long nowMs)
{
  // Ignore if too close to previous blink
  if (
    lastDetectedBlinkTime != 0 &&
    (nowMs - lastDetectedBlinkTime) < BLINK_DEBOUNCE_MS
  ) {
    return;
  }

  // If the previous sequence has expired,
  // start a completely new sequence.
  if (
    pendingBlinkCount > 0 &&
    (nowMs - lastDetectedBlinkTime) >
      BLINK_SEQUENCE_TIMEOUT_MS
  ) {

    pendingBlinkCount = 0;
  }

  // Start new sequence
  if (pendingBlinkCount == 0) {

    firstBlinkTime = nowMs;

    pendingBlinkCount = 1;

  } else {

    pendingBlinkCount++;
  }

  lastDetectedBlinkTime = nowMs;

  Serial.print("BLINK DETECTED -> Count = ");
  Serial.println(pendingBlinkCount);

  // Send real-time blink count over BLE
  sendBLECommand('B', (uint8_t)pendingBlinkCount);

  // Sync to Firebase directly over Wi-Fi Hotspot
  queueFirebaseEvent(FB_EVT_BLINK, pendingBlinkCount, 0);

  // Safety:
  // We only support 4 blink commands.
  // If more than 4 are detected, reset sequence.
  if (pendingBlinkCount > 4) {

    Serial.println(
      "More than 4 blinks -> sequence reset"
    );

    pendingBlinkCount = 0;
    sendBLECommand('R', 0);
  }
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
  Serial.begin(BAUD_RATE);

  delay(100);

  pinMode(INPUT_PIN, INPUT);

  pinMode(LED_BUILTIN, OUTPUT);

  // Startup LED
  digitalWrite(LED_BUILTIN, HIGH);

  delay(300);

  digitalWrite(LED_BUILTIN, LOW);

  // NeoPixel
  pixels.begin();

  pixels.clear();

  pixels.show();

  // ----------------------------------------------------------
  // WIFI HOTSPOT & FIREBASE BACKGROUND WORKER INIT
  // ----------------------------------------------------------
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println();
  Serial.printf("Connecting to Hotspot SSID: %s ...\n", WIFI_SSID);

  // Initialize FreeRTOS queue and worker task for Firebase
  firebaseQueue = xQueueCreate(16, sizeof(FirebaseEvent));
  if (firebaseQueue != nullptr) {
    xTaskCreate(
      firebaseTask,
      "firebaseTask",
      10240, // 10KB stack for WiFiClientSecure TLS operations
      nullptr,
      1,
      nullptr
    );
  }

  // ----------------------------------------------------------
  // BLE INIT
  // ----------------------------------------------------------

  BLEDevice::init("ESP32C6_EEG");

  pBleServer =
    BLEDevice::createServer();

  pBleServer->setCallbacks(
    new MyServerCallbacks()
  );

  pBlinkService =
    pBleServer->createService(
      BLINK_SERVICE_UUID
    );

  // Notify characteristic
  pBlinkChar =
    pBlinkService->createCharacteristic(
      BLINK_CHAR_UUID,
      BLECharacteristic::PROPERTY_NOTIFY
    );

  pBlinkChar->addDescriptor(
    new BLE2902()
  );

  pBlinkService->start();

  // Start BLE advertising
  BLEAdvertising* pAdvertising =
    pBleServer->getAdvertising();

  pAdvertising->addServiceUUID(BLINK_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections
  pAdvertising->setMinPreferred(0x12);
  pAdvertising->start();

  Serial.println();
  Serial.println("================================");
  Serial.println("ESP32C6 EEG SYSTEM READY");
  Serial.println("================================");

  Serial.println("Blink controls:");
  Serial.println("1 blink  = NEXT");
  Serial.println("2 blinks = SPEAK");
  Serial.println("3 blinks = NOTHING");
  Serial.println("4 blinks = SYSTEM ON/OFF");

  Serial.println("================================");
  Serial.println("System starts OFF");
  Serial.println("================================");
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop()
{
  // ==========================================================
  // EEG SAMPLING
  // ==========================================================

  static unsigned long lastMicros = micros();

  unsigned long nowMicros = micros();

  unsigned long dt =
    nowMicros - lastMicros;

  lastMicros = nowMicros;

  static long timer = 0;

  timer -= dt;

  if (timer <= 0) {

    timer +=
      1000000L / SAMPLE_RATE;

    int raw =
      analogRead(INPUT_PIN);

    float filt =
      EEGFilter(
        Notch(raw)
      );

    float filtered =
      highpass(filt);

    currentEEGEnvelope =
      updateEEGEnvelope(filtered);
  }

  // ==========================================================
  // BLINK DETECTION
  // ==========================================================

  unsigned long nowMs =
    millis();

  bool thresholdHigh =
    currentEEGEnvelope > BlinkThreshold;

  // ----------------------------------------------------------
  // Detect ONLY rising edge
  //
  // This is important.
  //
  // If the EEG signal stays above threshold for 500 ms,
  // it should NOT become 2 or 3 blinks.
  // One threshold crossing = one blink.
  // ----------------------------------------------------------

  if (
    thresholdHigh &&
    !thresholdWasHigh
  ) {

    registerBlink(nowMs);
  }

  thresholdWasHigh =
    thresholdHigh;

  // ==========================================================
  // WAIT FOR BLINK SEQUENCE TO FINISH
  // ==========================================================

  if (
    pendingBlinkCount > 0 &&
    (nowMs - lastDetectedBlinkTime) >
      BLINK_SEQUENCE_TIMEOUT_MS
  ) {

    int completedCount =
      pendingBlinkCount;

    // Reset BEFORE executing.
    // This prevents accidental retriggering.
    pendingBlinkCount = 0;

    Serial.print(
      "Executing completed sequence: "
    );

    Serial.println(completedCount);

    executeBlinkSequence(
      completedCount
    );
  }

  // ==========================================================
  // SYSTEM AUTO TIMEOUT
  // ==========================================================

  if (
    menu &&
    (nowMs - menuStartTime) >
      MENU_TIMEOUT_MS
  ) {

    Serial.println();
    Serial.println(
      "SYSTEM TIMEOUT -> OFF"
    );

    systemOFF();
  }

  // ==========================================================
  // SMALL LOOP DELAY
  // ==========================================================

  delay(1);
}