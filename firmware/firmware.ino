/*
 * NeuroSpeak - EOG Eye-Blink Assistive Controller (WIRED-UP VERSION)
 * Hardware: ESP32-C6 + Upside Down Labs NPG Lite
 *
 * WHAT CHANGED FROM YOUR REPO'S firmware.ino
 * -------------------------------------------
 * Your repo already contains a well-built adaptive blink detector
 * (BlinkDetector.cpp + AdaptiveThreshold.cpp + Calibration.cpp +
 * Filters.cpp + StateMachine.cpp + SequenceClassifier.cpp) - but
 * firmware.ino never included or instantiated any of it. The .ino that
 * Arduino actually compiles had its own separate, much cruder inline
 * detector with a hardcoded BlinkThreshold=50.0 and no falling-edge logic.
 * That's the file you've been tuning. This version deletes that dead
 * inline detector and actually calls BlinkDetector::process() and
 * SequenceClassifier, so the calibration/adaptive-threshold/FSM code you
 * already wrote is the code that's actually running on the board.
 *
 * Your mapping, preserved exactly:
 *   4 blinks -> System ON / OFF (toggle)
 *   1 blink  -> Next menu item   (only when system ON)
 *   2 blinks -> Select / speak   (only when system ON)
 *
 * NOTE ON SAMPLE RATE: Filters.cpp's LPF coefficients and
 * BlinkDetector.cpp's kSamplePeriodSeconds are both derived for 500Hz
 * (2ms/sample) - NOT 512Hz. Sampling at the wrong rate shifts the filter's
 * real cutoff frequency away from what was designed. This version samples
 * at exactly 500Hz to match your own filter code.
 *
 * DEBUG WORKFLOW - do this before assuming anything else is wrong:
 *   Set DEBUG_MODE to DEBUG_PLOT, flash, open Serial Plotter at 250000 baud.
 *   Sit still through calibration (4s), then blink deliberately several
 *   times. Confirm "Filtered" clearly crosses "Trigger" on every blink.
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>

#include "BlinkDetector.h"
#include "SequenceClassifier.h"

// -----------------------------------------------------------------------------
// DEBUG MODE - start here
// -----------------------------------------------------------------------------
enum DebugMode : uint8_t { DEBUG_OFF, DEBUG_EVENTS, DEBUG_PLOT };
constexpr DebugMode DEBUG_MODE = DEBUG_PLOT;   // <-- flash with this first

// -----------------------------------------------------------------------------
// Network & Hardware Configuration
// -----------------------------------------------------------------------------
constexpr char WIFI_SSID[] = "Neuro";
constexpr char WIFI_PASSWORD[] = "123@#$456";
constexpr char FIREBASE_HOST[] = "neurospeak2-default-rtdb.firebaseio.com";
constexpr char FIREBASE_API_KEY[] = "AIzaSyCIWR_XGD-UGltZge3hIoDmqtGavsXLFcs";

constexpr char SERVICE_UUID[] = "6910123a-eb0d-4c35-9a60-bebe1dcb549d";
constexpr char CHARACTERISTIC_UUID[] = "5f4f1107-7fc1-43b2-a540-0aa1a9f1ce78";
constexpr uint8_t BLE_SYSTEM_ACTIVATED = 0;
constexpr uint8_t BLE_SYSTEM_INACTIVE = 127;

#define INPUT_PIN A0
constexpr uint32_t SAMPLE_RATE = 500;   // MUST match Filters.cpp / BlinkDetector.cpp (2ms period)
constexpr uint32_t SAMPLE_INTERVAL_US = 1000000UL / SAMPLE_RATE;
constexpr uint32_t MENU_TIMEOUT_MS = 10000;   // 10s inactivity -> auto OFF

const char *OPTION_NAMES[6] = {
    "Food", "Help", "Outing", "Television", "Washroom", "Water"};

// -----------------------------------------------------------------------------
// BLE Global Controls
// -----------------------------------------------------------------------------
BLEServer *pServer = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
BLEAdvertising *pAdvertising = nullptr;
bool deviceConnected = false;
volatile bool advertisingRestartPending = false;

// -----------------------------------------------------------------------------
// Blink Detection + Sequence Classification (the actual, previously-unused code)
// -----------------------------------------------------------------------------
BlinkDetector blinkDetector;
SequenceClassifier sequenceClassifier;

// -----------------------------------------------------------------------------
// Menu / System State
// -----------------------------------------------------------------------------
enum SystemState : uint8_t { SYSTEM_OFF, SYSTEM_ON };
SystemState currentState = SYSTEM_OFF;

constexpr uint8_t FIRST_MENU_ITEM = 1;
constexpr uint8_t MAX_MENU_ITEMS = 6;
uint8_t currentMenuIndex = FIRST_MENU_ITEM;
uint32_t lastActivityMs = 0;

struct TelemetryData {
  uint8_t blinkCount;
  bool systemActivated;
  char selectedOutput[16];
};
TelemetryData currentTelemetry = {0, false, "Food"};
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

const char *systemStateName() { return currentState == SYSTEM_ON ? "ON" : "OFF"; }

void updateTelemetry(uint8_t blinkCount) {
  portENTER_CRITICAL(&telemetryMux);
  currentTelemetry.blinkCount = blinkCount;
  currentTelemetry.systemActivated = (currentState == SYSTEM_ON);
  uint8_t idx = (currentMenuIndex >= 1 && currentMenuIndex <= 6) ? (currentMenuIndex - 1) : 0;
  strncpy(currentTelemetry.selectedOutput, OPTION_NAMES[idx], sizeof(currentTelemetry.selectedOutput) - 1);
  currentTelemetry.selectedOutput[sizeof(currentTelemetry.selectedOutput) - 1] = '\0';
  portEXIT_CRITICAL(&telemetryMux);
}

// -----------------------------------------------------------------------------
// BLE Callbacks
// -----------------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    deviceConnected = true;
    if (DEBUG_MODE != DEBUG_OFF) Serial.println("BLE: Client connected");
  }
  void onDisconnect(BLEServer *server) override {
    deviceConnected = false;
    advertisingRestartPending = true;
  }
};

class DataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    if (value.length() >= 2 && DEBUG_MODE != DEBUG_OFF) {
      Serial.printf("Web ACK -> Tag: %c, Index: %u\n", value[0], (uint8_t)value[1]);
    }
  }
};

void sendBLEMenuAction(char action, uint8_t index) {
  if (!deviceConnected) return;
  if (action == 'X') {
    uint8_t payload[1] = {index};
    pCharacteristic->setValue(payload, 1);
  } else {
    uint8_t payload[2] = {static_cast<uint8_t>(action), index};
    pCharacteristic->setValue(payload, 2);
  }
  pCharacteristic->notify();
}

// -----------------------------------------------------------------------------
// Command execution - your original 1/2/4 blink mapping, unchanged
// -----------------------------------------------------------------------------
void executeCommand(BlinkCommand command, uint32_t nowMs) {
  switch (command) {
    case BlinkCommand::Quad:
      currentState = (currentState == SYSTEM_OFF) ? SYSTEM_ON : SYSTEM_OFF;
      sendBLEMenuAction('X', currentState == SYSTEM_ON ? BLE_SYSTEM_ACTIVATED : BLE_SYSTEM_INACTIVE);
      if (currentState == SYSTEM_ON) currentMenuIndex = FIRST_MENU_ITEM;
      lastActivityMs = nowMs;
      if (DEBUG_MODE != DEBUG_OFF) {
        Serial.println(currentState == SYSTEM_ON ? ">>> System Activated (ON) <<<" : ">>> System Deactivated (OFF) <<<");
      }
      updateTelemetry(4);
      break;

    case BlinkCommand::Single:
      if (currentState == SYSTEM_ON) {
        currentMenuIndex = (currentMenuIndex % MAX_MENU_ITEMS) + 1;
        sendBLEMenuAction('S', currentMenuIndex);
        lastActivityMs = nowMs;
        if (DEBUG_MODE != DEBUG_OFF) Serial.printf(">>> Menu -> %s <<<\n", OPTION_NAMES[currentMenuIndex - 1]);
        updateTelemetry(1);
      } else if (DEBUG_MODE != DEBUG_OFF) {
        Serial.println("Ignored: System OFF");
      }
      break;

    case BlinkCommand::Double:
      if (currentState == SYSTEM_ON) {
        sendBLEMenuAction('A', currentMenuIndex);
        lastActivityMs = nowMs;
        if (DEBUG_MODE != DEBUG_OFF) Serial.printf(">>> Select -> %s <<<\n", OPTION_NAMES[currentMenuIndex - 1]);
        updateTelemetry(2);
      } else if (DEBUG_MODE != DEBUG_OFF) {
        Serial.println("Ignored: System OFF");
      }
      break;

    default:
      break;
  }
}

// -----------------------------------------------------------------------------
// Firebase Realtime Database Task (/live_data) - unchanged from your repo
// -----------------------------------------------------------------------------
int sendFirebaseRequest(const char *nodePath, const String &payload, const char *httpMethod) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  String urlAuth = String("https://") + FIREBASE_HOST + nodePath + ".json?auth=" + FIREBASE_API_KEY;
  int httpCode = -1;
  WiFiClientSecure secClient;
  secClient.setInsecure();
  HTTPClient http;
  http.setTimeout(6000);
  if (http.begin(secClient, urlAuth)) {
    http.addHeader("Content-Type", "application/json");
    httpCode = http.sendRequest(httpMethod, payload);
    http.end();
  }
  return httpCode;
}

void firebaseTask(void *pvParameters) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));

  uint32_t lastLiveStreamMs = 0;
  uint8_t lastStreamedBlinkCount = 255;
  bool lastStreamedSystemActivated = false;
  char lastStreamedSelectedOutput[16] = "";

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) continue;

    TelemetryData dataCopy;
    portENTER_CRITICAL(&telemetryMux);
    dataCopy = currentTelemetry;
    portEXIT_CRITICAL(&telemetryMux);

    const bool stateChanged = (dataCopy.blinkCount != lastStreamedBlinkCount) ||
                              (dataCopy.systemActivated != lastStreamedSystemActivated) ||
                              (strcmp(dataCopy.selectedOutput, lastStreamedSelectedOutput) != 0);

    if (stateChanged || (now - lastLiveStreamMs >= 2000)) {
      lastLiveStreamMs = now;
      lastStreamedBlinkCount = dataCopy.blinkCount;
      lastStreamedSystemActivated = dataCopy.systemActivated;
      strncpy(lastStreamedSelectedOutput, dataCopy.selectedOutput, sizeof(lastStreamedSelectedOutput) - 1);

      String payload = "{";
      payload += "\"blinkCount\":" + String(dataCopy.blinkCount) + ",";
      payload += "\"systemActivated\":" + String(dataCopy.systemActivated ? "true" : "false") + ",";
      payload += "\"selectedOutput\":\"" + String(dataCopy.selectedOutput) + "\"";
      payload += "}";
      sendFirebaseRequest("/live_data", payload, "PUT");
    }
  }
}

// -----------------------------------------------------------------------------
// Setup
// -----------------------------------------------------------------------------
uint32_t lastSampleUs = 0;

void setup() {
  Serial.begin(DEBUG_MODE == DEBUG_PLOT ? 250000 : 115200);
  delay(100);
  pinMode(INPUT_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(INPUT_PIN, ADC_11db);

  blinkDetector.begin();

  BLEDevice::init("ESP32C6_EEG");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *service = pServer->createService(SERVICE_UUID);
  pCharacteristic = service->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_NOTIFY |
          BLECharacteristic::PROPERTY_INDICATE);
  pCharacteristic->addDescriptor(new BLE2902());
  pCharacteristic->setCallbacks(new DataCallbacks());
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  pAdvertising = advertising;
  BLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setCompleteServices(BLEUUID(SERVICE_UUID));
  advertising->setAdvertisementData(advData);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  xTaskCreatePinnedToCore(firebaseTask, "FirebaseTask", 8192, nullptr, 1, nullptr, 0);

  lastSampleUs = micros();
  lastActivityMs = millis();

  if (DEBUG_MODE != DEBUG_OFF) {
    Serial.println("=====================================================");
    Serial.println("NeuroSpeak EOG Controller - BlinkDetector WIRED IN");
    Serial.println("Calibrating - sit still, eyes open...");
    Serial.println("=====================================================");
  }
}

// -----------------------------------------------------------------------------
// Loop
// -----------------------------------------------------------------------------
void loop() {
  const uint32_t nowUs = micros();
  const uint32_t nowMs = millis();

  if (advertisingRestartPending) {
    advertisingRestartPending = false;
    if (pAdvertising != nullptr) pAdvertising->start();
  }

  if (static_cast<uint32_t>(nowUs - lastSampleUs) >= SAMPLE_INTERVAL_US) {
    lastSampleUs += SAMPLE_INTERVAL_US;

    const uint16_t raw = static_cast<uint16_t>(analogRead(INPUT_PIN));
    const bool blinkAccepted = blinkDetector.process(raw, nowMs);

    if (blinkAccepted) {
      BlinkCommand cmd = sequenceClassifier.add(nowMs);
      if (cmd != BlinkCommand::None) executeCommand(cmd, nowMs);
      if (DEBUG_MODE != DEBUG_OFF) {
        Serial.printf("Blink accepted | seq_count=%u conf=%.0f%%\n",
                      sequenceClassifier.count(), blinkDetector.confidence());
      }
    }

    static uint32_t lastDebugMs = 0;
    if (DEBUG_MODE == DEBUG_PLOT) {
      Serial.printf("Raw:%u,Filtered:%.1f,Trigger:%.1f,Baseline:%.1f\n",
                    raw, blinkDetector.filtered(),
                    blinkDetector.calibrated() ? blinkDetector.trigger() : 0,
                    blinkDetector.calibrated() ? blinkDetector.baseline() : 0);
    } else if (DEBUG_MODE == DEBUG_EVENTS && (nowMs - lastDebugMs) >= 200) {
      lastDebugMs = nowMs;
      if (!blinkDetector.calibrated()) {
        Serial.printf("[CALIBRATING] %u/%u\n", blinkDetector.calibrationCount(), Calibration::kSamples);
      } else {
        Serial.printf("Filtered:%.1f Baseline:%.1f Trig:%.1f State:%s System:%s\n",
                      blinkDetector.filtered(), blinkDetector.baseline(), blinkDetector.trigger(),
                      detectorStateName(blinkDetector.state()), systemStateName());
      }
    }
  }

  // Finalize single/double sequences after the classifier's own silence window
  BlinkCommand pending = sequenceClassifier.poll(nowMs);
  if (pending != BlinkCommand::None) executeCommand(pending, nowMs);

  if (currentState == SYSTEM_ON && (nowMs - lastActivityMs) > MENU_TIMEOUT_MS) {
    currentState = SYSTEM_OFF;
    sendBLEMenuAction('X', BLE_SYSTEM_INACTIVE);
    updateTelemetry(0);
    lastActivityMs = nowMs;
    if (DEBUG_MODE != DEBUG_OFF) Serial.println("10s Inactivity Timeout -> System OFF");
  }
}
