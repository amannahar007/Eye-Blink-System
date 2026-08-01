/*
 * NeuroSpeak - High-Precision Biomedical EOG Eye-Blink Assistive Controller
 * Hardware: ESP32-C6 Microcontroller + Upside Down Labs NPG Lite / BioAmp EXG Pill
 *
 * Electrode Placement (Official M2W Configuration):
 *   - IN+  (Non-inverting / +ve): Middle of Forehead (Fpz) or Above Eye
 *   - IN-  (Inverting / -ve)    : Bony part behind ear (Mastoid Process A1) or Below Eye
 *   - REF  (Reference / Ground) : Opposite Mastoid Process (A2)
 *
 * Electrophysiological Principles:
 *   The eye functions as an electric dipole (cornea positive relative to retina).
 *   During a blink, Bell's phenomenon causes the eyeball to rotate upwards,
 *   generating a prominent positive potential transient (+100mV to +800mV scaled).
 *
 * Signal Processing Pipeline (Production Engineering Refactor):
 *   1. Fixed-Phase Sample Clock: 250 Hz sampling (4000 µs) with zero cumulative jitter.
 *   2. Median Filter Warmup (N=3): Rejects impulse noise without 0.0 startup step transients.
 *   3. Baseline-Subtracted 0.5Hz IIR HPF: Eliminates DC offset and electrode polarization drift.
 *   4. Precision 50Hz Biquad Notch Filter: Suppresses powerline electromagnetic interference.
 *   5. 12Hz 2nd-Order Low-Pass Butterworth Filter: Removes EMG muscle artifacts and high-freq noise.
 *   6. Smoothed Teager-Kaiser Energy Profile (TKEO): Enhances blink peak localization.
 *   7. Outlier-Resistant Boot Calibration: 5-second trimmed mean & 2.5-sigma variance estimation.
 *   8. Gated Continuous Baseline Tracking (EMV): Baseline updates ONLY during quiet idle states.
 *   9. Dynamic Hysteresis Thresholding: Dynamic trigger and release thresholds scaled by sigma.
 *  10. 6-State Finite State Machine (FSM): READY -> RISING -> PEAK -> FALLING -> REFRACTORY.
 *  11. Multi-Feature Bio-Confidence Engine (100 Points): Evaluates SNR, physiological duration,
 *      bio-asymmetry (fast rise / slow fall ratio), and derivative variance (EMG rejection).
 *  12. Optimized Sequence Classifier: Low-latency 420ms inter-blink window with instant 4-blink dispatch.
 *  13. Thread-Safe BLE & Firebase RTDB (/live_data) Integration.
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

constexpr uint8_t SENSOR_PIN = 0;               // GPIO0 / ADC1_CH0 on ESP32-C6
constexpr uint32_t SAMPLE_INTERVAL_US = 2000;   // 500 Hz exact sample clock
constexpr uint32_t DEBUG_INTERVAL_MS = 20;      // 50 Hz Plotter telemetry rate

// Uncomment for compact Arduino Serial Plotter-compatible detector telemetry.
// #define DEBUG_BLINK

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
volatile int lastFirebaseHttpCode = -1;  // Debug telemetry only; Firebase payload is unchanged.
uint8_t lastBleAction = 0;
uint8_t lastBleIndex = 0;

constexpr uint32_t COMMAND_LOCKOUT_MS = 400;

// -----------------------------------------------------------------------------
// State Machine Definitions
// -----------------------------------------------------------------------------
enum SystemState : uint8_t {
  SYSTEM_OFF,
  SYSTEM_ON
};

SystemState currentState = SYSTEM_OFF;
BlinkDetector blinkDetector;
SequenceClassifier sequenceClassifier;

// Sequence tracking
constexpr uint8_t FIRST_MENU_ITEM = 1;
constexpr uint8_t MAX_MENU_ITEMS = 6;
uint8_t currentMenuIndex = FIRST_MENU_ITEM;

bool commandLocked = false;
uint32_t commandLockoutStartMs = 0;

uint32_t lastSampleUs = 0;
uint32_t lastDebugMs = 0;
uint32_t sampleDeadlineMisses = 0;
uint32_t worstSampleLatenessUs = 0;

// Thread-Safe Telemetry Snapshot for Firebase
struct TelemetryData {
  uint8_t blinkCount;
  bool systemActivated;
  char selectedOutput[16];
};

TelemetryData currentTelemetry = {0, false, "Food"};
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

// -----------------------------------------------------------------------------
// Helper Functions
// -----------------------------------------------------------------------------
const char *systemStateName() {
  return currentState == SYSTEM_ON ? "ON" : "OFF";
}

inline bool elapsed(uint32_t now, uint32_t since, uint32_t duration) {
  return static_cast<uint32_t>(now - since) >= duration;
}

inline float medianOfThree(float a, float b, float c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

// Quick select / partition algorithm for fast trimmed mean computation
void quickSelect(float arr[], int l, int r, int k) {
  while (l < r) {
    float pivot = arr[r];
    int i = l - 1;
    for (int j = l; j < r; j++) {
      if (arr[j] <= pivot) {
        i++;
        float t = arr[i]; arr[i] = arr[j]; arr[j] = t;
      }
    }
    float t = arr[i + 1]; arr[i + 1] = arr[r]; arr[r] = t;
    int pivotIdx = i + 1;

    if (pivotIdx == k) return;
    else if (pivotIdx < k) l = pivotIdx + 1;
    else r = pivotIdx - 1;
  }
}

// -----------------------------------------------------------------------------
// BLE Callbacks
// -----------------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    deviceConnected = true;
    Serial.println("BLE: Client connected");
  }

  void onDisconnect(BLEServer *server) override {
    deviceConnected = false;
    advertisingRestartPending = true;
    Serial.println("BLE: Client disconnected; advertising scheduled");
  }
};

class DataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    if (value.length() < 2) return;

    const uint8_t tag = static_cast<uint8_t>(value[0]);
    const uint8_t idx = static_cast<uint8_t>(value[1]);
    const bool idxValid = (idx >= 1 && idx <= 6);

    switch (tag) {
      case 's':
        Serial.printf("Web ACK: Menu highlight -> %s (idx %u)\n", idxValid ? OPTION_NAMES[idx - 1] : "?", idx);
        break;
      case 'a':
        Serial.printf("Web ACK: Selected -> %s (idx %u)\n", idxValid ? OPTION_NAMES[idx - 1] : "?", idx);
        break;
      case 'x':
        Serial.printf("Web ACK: System %s\n", idx ? "ACTIVE" : "INACTIVE");
        break;
    }
  }
};

void sendBLEMenuAction(char action, uint8_t index) {
  if (!deviceConnected) return;
  uint8_t payload[2] = {static_cast<uint8_t>(action), index};
  pCharacteristic->setValue(payload, sizeof(payload));
  pCharacteristic->notify();
  lastBleAction = static_cast<uint8_t>(action);
  lastBleIndex = index;
}

// -----------------------------------------------------------------------------
// Optimized Sequence Classifier & Menu FSM
// -----------------------------------------------------------------------------
void executeBlinkCommand(BlinkCommand command, uint32_t nowMs) {
  if (commandLocked) {
    Serial.println("Blink ignored: Command lockout active");
    return;
  }
  const char *executedCommand = "Ignored: Unsupported sequence";

  switch (command) {
    case BlinkCommand::Single:
      if (currentState == SYSTEM_ON) {
        currentMenuIndex = (currentMenuIndex % MAX_MENU_ITEMS) + 1;
        sendBLEMenuAction('S', currentMenuIndex);
        executedCommand = "Rotate menu";
      } else {
        executedCommand = "Ignored: System OFF";
      }
      break;

    case BlinkCommand::Double:
      if (currentState == SYSTEM_ON) {
        sendBLEMenuAction('A', currentMenuIndex);
        executedCommand = "Select highlighted option";
      } else {
        executedCommand = "Ignored: System OFF";
      }
      break;

    case BlinkCommand::Quad:
      currentState = (currentState == SYSTEM_OFF) ? SYSTEM_ON : SYSTEM_OFF;
      sendBLEMenuAction('X', currentState == SYSTEM_ON ? BLE_SYSTEM_ACTIVATED : BLE_SYSTEM_INACTIVE);
      executedCommand = (currentState == SYSTEM_ON) ? "System Activated (ON)" : "System Deactivated (OFF)";
      break;

    default:
      break;
  }

  commandLocked = true;
  commandLockoutStartMs = nowMs;
  Serial.printf(">>> EXECUTE COMMAND: %s <<<\n", executedCommand);
}

// -----------------------------------------------------------------------------
// Firebase Realtime Database Task (/live_data)
// -----------------------------------------------------------------------------
int sendFirebaseRequest(const char *nodePath, const String &payload, const char *httpMethod) {
  if (WiFi.status() != WL_CONNECTED) {
    return -1;
  }

  String urlNoAuth = String("https://") + FIREBASE_HOST + nodePath + ".json";
  String urlAuth = String("https://") + FIREBASE_HOST + nodePath + ".json?auth=" + FIREBASE_API_KEY;

  int httpCode = -1;

  {
    WiFiClientSecure secClient;
    secClient.setInsecure();
    HTTPClient http;
    http.setTimeout(6000);

    if (http.begin(secClient, urlNoAuth)) {
      http.addHeader("Content-Type", "application/json");
      httpCode = http.sendRequest(httpMethod, payload);
      http.end();
    }
  }

  if (httpCode != 200 && httpCode != 201) {
    WiFiClientSecure secClient;
    secClient.setInsecure();
    HTTPClient http;
    http.setTimeout(6000);

    if (http.begin(secClient, urlAuth)) {
      http.addHeader("Content-Type", "application/json");
      int retryCode = http.sendRequest(httpMethod, payload);
      if (retryCode == 200 || retryCode == 201) {
        httpCode = retryCode;
      }
      http.end();
    }
  }

  return httpCode;
}

void firebaseTask(void *pvParameters) {
  Serial.printf("Wi-Fi: Connecting to SSID '%s'...\n", WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected! IP address: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWi-Fi connection pending. Continuing background reconnect...");
  }

  uint32_t lastLiveStreamMs = 0;
  uint32_t lastFbReportMs = 0;
  uint32_t lastWifiRetryMs = 0;

  uint8_t lastStreamedBlinkCount = 255;
  bool lastStreamedSystemActivated = false;
  char lastStreamedSelectedOutput[16] = "";

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (now - lastWifiRetryMs >= 5000) {
        lastWifiRetryMs = now;
        Serial.printf("Wi-Fi: Reconnecting to '%s'...\n", WIFI_SSID);
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

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
      lastStreamedSelectedOutput[sizeof(lastStreamedSelectedOutput) - 1] = '\0';

      String payload = "{";
      payload += "\"blinkCount\":" + String(dataCopy.blinkCount) + ",";
      payload += "\"systemActivated\":" + String(dataCopy.systemActivated ? "true" : "false") + ",";
      payload += "\"selectedOutput\":\"" + String(dataCopy.selectedOutput) + "\"";
      payload += "}";

      int httpCode = sendFirebaseRequest("/live_data", payload, "PUT");
      lastFirebaseHttpCode = httpCode;

      if (now - lastFbReportMs >= 3000) {
        lastFbReportMs = now;
        if (httpCode == 200) {
          Serial.println("Firebase RTDB: /live_data updated successfully (HTTP 200)");
        } else {
          Serial.printf("Firebase RTDB Stream Status: HTTP %d\n", httpCode);
        }
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Arduino Setup & Main Execution Loop
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(SENSOR_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);

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

  BLEAdvertisementData scanResponseData;
  scanResponseData.setName("ESP32C6_EEG");
  advertising->setScanResponseData(scanResponseData);

  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinInterval(0x20);
  advertising->setMaxInterval(0x40);
  BLEDevice::startAdvertising();

  xTaskCreatePinnedToCore(
      firebaseTask,
      "FirebaseTask",
      8192,
      nullptr,
      1,
      nullptr,
      0
  );

  blinkDetector.begin();
  lastSampleUs = micros();

  Serial.println("=========================================================================");
  Serial.println("NeuroSpeak Biomedical EOG Controller (ESP32-C6 Optimized)");
  Serial.println("Electrode Setup: IN+(Forehead Midline), IN-(Mastoid A1), Ref(Mastoid A2)");
  Serial.println("Robust auto-calibration running (4 seconds)... Please remain still.");
  Serial.println("=========================================================================");
}

void loop() {
  const uint32_t nowUs = micros();
  const uint32_t nowMs = millis();

  if (advertisingRestartPending) {
    advertisingRestartPending = false;
    if (pAdvertising != nullptr) {
      pAdvertising->start();
      Serial.println("BLE: Advertising restarted");
    }
  }

  if (commandLocked && elapsed(nowMs, commandLockoutStartMs, COMMAND_LOCKOUT_MS)) {
    commandLocked = false;
  }

  // Fixed-rate 500 Hz sampling. A late loop skips stale sample slots rather than
  // bursting conversions; deadline loss is recorded for clinical debugging.
  if (static_cast<uint32_t>(nowUs - lastSampleUs) >= SAMPLE_INTERVAL_US) {
    const uint32_t latenessUs = nowUs - lastSampleUs;
    if (latenessUs >= 2 * SAMPLE_INTERVAL_US) {
      sampleDeadlineMisses += latenessUs / SAMPLE_INTERVAL_US - 1;
      lastSampleUs = nowUs;
    } else {
      lastSampleUs += SAMPLE_INTERVAL_US;
    }
    if (latenessUs > worstSampleLatenessUs) worstSampleLatenessUs = latenessUs;

    const uint16_t rawValue = static_cast<uint16_t>(analogRead(SENSOR_PIN));
    const bool blinkDetected = blinkDetector.process(rawValue, nowMs);

    if (blinkDetected) {
      const BlinkCommand command = sequenceClassifier.add(nowMs);
      if (command != BlinkCommand::None) executeBlinkCommand(command, nowMs);
    }

    // Thread-safe snapshot for FreeRTOS task
    portENTER_CRITICAL(&telemetryMux);
    currentTelemetry.blinkCount = sequenceClassifier.count();
    currentTelemetry.systemActivated = (currentState == SYSTEM_ON);
    uint8_t idx = (currentMenuIndex >= 1 && currentMenuIndex <= 6) ? (currentMenuIndex - 1) : 0;
    strncpy(currentTelemetry.selectedOutput, OPTION_NAMES[idx], sizeof(currentTelemetry.selectedOutput) - 1);
    currentTelemetry.selectedOutput[sizeof(currentTelemetry.selectedOutput) - 1] = '\0';
    portEXIT_CRITICAL(&telemetryMux);

    #ifdef DEBUG_BLINK
    if (elapsed(nowMs, lastDebugMs, DEBUG_INTERVAL_MS)) {
      lastDebugMs = nowMs;
      // Named numeric fields are accepted by Arduino Serial Plotter.
      Serial.printf("Raw:%u Filtered:%.2f Baseline:%.2f Noise:%.2f Threshold:%.2f NegThreshold:%.2f Peak:%.2f Prominence:%.2f NegPeak:%.2f Width:%u Duration:%u Energy:%.3f Confidence:%.1f Accepted:%u Rejected:%u Reason:%u FSM:%u Menu:%u BLEAction:%u BLEIndex:%u Firebase:%d Missed:%lu LateUs:%lu\n",
                    rawValue, blinkDetector.filtered(), blinkDetector.baseline(), blinkDetector.noise(),
                    blinkDetector.trigger(), blinkDetector.negativeTrigger(), blinkDetector.peak(),
                    blinkDetector.prominence(), blinkDetector.negativePeak(), blinkDetector.widthMs(), blinkDetector.widthMs(), blinkDetector.energy(),
                    blinkDetector.confidence(), blinkDetected ? 1 : 0,
                    blinkDetector.rejectReason() == BlinkRejectReason::None ? 0 : 1,
                    static_cast<unsigned>(blinkDetector.rejectReason()),
                    static_cast<unsigned>(blinkDetector.state()), currentMenuIndex, lastBleAction, lastBleIndex, lastFirebaseHttpCode,
                    sampleDeadlineMisses, worstSampleLatenessUs);
    }
    #endif
  }

  const BlinkCommand completed = sequenceClassifier.poll(nowMs);
  if (completed != BlinkCommand::None) executeBlinkCommand(completed, nowMs);
}
