/*
 * NeuroSpeak - robust EOG eye-blink controller for ESP32-C6 with Firebase RTDB live streaming
 *
 * Commands (one completed blink sequence at a time):
 *   1 blink  -> move the menu highlight
 *   2 blinks -> speak/select the highlighted item
 *   4 blinks -> toggle the system and announce its new state
 *
 * The sketch is deliberately non-blocking.  Sampling, blink validation,
 * sequence timing, BLE, and command execution are each driven by millis()/micros().
 * Firebase Realtime Database telemetry runs asynchronously on a dedicated FreeRTOS background task.
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// -----------------------------------------------------------------------------
// Wi-Fi and Firebase Realtime Database credentials
// -----------------------------------------------------------------------------
constexpr char WIFI_SSID[] = "Neuro";
constexpr char WIFI_PASSWORD[] = "123@#$456";
constexpr char FIREBASE_HOST[] = "neurospeak2-default-rtdb.firebaseio.com";
constexpr char FIREBASE_API_KEY[] = "AIzaSyCIWR_XGD-UGltZge3hIoDmqtGavsXLFcs";

// -----------------------------------------------------------------------------
// BLE protocol.  Keep these UUIDs in sync with M2W-main/src/components/Mainpage.tsx.
// -----------------------------------------------------------------------------
constexpr char SERVICE_UUID[] = "6910123a-eb0d-4c35-9a60-bebe1dcb549d";
constexpr char CHARACTERISTIC_UUID[] = "5f4f1107-7fc1-43b2-a540-0aa1a9f1ce78";
constexpr uint8_t BLE_SYSTEM_ACTIVATED = 0;
constexpr uint8_t BLE_SYSTEM_INACTIVE = 127;

BLEServer *pServer = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
BLEAdvertising *pAdvertising = nullptr;
bool deviceConnected = false;
volatile bool advertisingRestartPending = false;

// Human-readable names, in the SAME order as the `options` array in
// M2W-main/src/components/Mainpage.tsx (food, help, outing, television,
// washroom, water).
const char *OPTION_NAMES[6] = {
    "Food", "Help", "Outing", "Television", "Washroom", "Water"};

// -----------------------------------------------------------------------------
// Hardware and sampling.  GPIO0 is A0/ADC1_CH0 on the ESP32-C6 Dev Module.
// -----------------------------------------------------------------------------
constexpr uint8_t SENSOR_PIN = 0;
constexpr uint32_t SAMPLE_INTERVAL_US = 4000;  // 250 Hz
constexpr uint32_t DEBUG_INTERVAL_MS = 100;    // Serial output rate: 10 Hz

// -----------------------------------------------------------------------------
// Signal conditioning.
// -----------------------------------------------------------------------------
constexpr float BASELINE_ALPHA = 0.004f;  // Smaller = more rejection of slow drift
constexpr float LOW_PASS_ALPHA = 0.22f;   // Smaller = smoother, but more latency

float rawHistory[3] = {0.0f, 0.0f, 0.0f};
uint8_t rawHistoryIndex = 0;
uint8_t rawHistoryCount = 0;
bool filterReady = false;
float baseline = 0.0f;
float filteredSignal = 0.0f;

// -----------------------------------------------------------------------------
// Blink validation.
// -----------------------------------------------------------------------------
constexpr float DEFAULT_BLINK_TRIGGER_THRESHOLD = 250.0f;
constexpr float DEFAULT_BLINK_RELEASE_THRESHOLD = 120.0f;  // Must be below trigger threshold
constexpr uint32_t MIN_BLINK_DURATION_MS = 35;
constexpr uint32_t MAX_BLINK_DURATION_MS = 450;    // Longer closures are ignored
constexpr uint32_t BLINK_REFRACTORY_MS = 120;      // Debounces successive blinks

float blinkTriggerThreshold = DEFAULT_BLINK_TRIGGER_THRESHOLD;
float blinkReleaseThreshold = DEFAULT_BLINK_RELEASE_THRESHOLD;

// -----------------------------------------------------------------------------
// Sequence timing.
// -----------------------------------------------------------------------------
constexpr uint32_t INTER_BLINK_TIMEOUT_MS = 650;
constexpr uint32_t MAX_SEQUENCE_WINDOW_MS = 1800;
constexpr uint32_t COMMAND_LOCKOUT_MS = 800;

// -----------------------------------------------------------------------------
// Menu state.
// -----------------------------------------------------------------------------
constexpr uint8_t FIRST_MENU_ITEM = 1;
constexpr uint8_t MAX_MENU_ITEMS = 6;
uint8_t currentMenuIndex = FIRST_MENU_ITEM;

enum SystemState : uint8_t {
  SYSTEM_OFF,
  SYSTEM_ON
};

enum BlinkDetectorState : uint8_t {
  DETECTOR_READY,
  DETECTOR_TRACKING_PULSE,
  DETECTOR_LONG_CLOSURE,
  DETECTOR_REFRACTORY
};

SystemState currentState = SYSTEM_OFF;
BlinkDetectorState detectorState = DETECTOR_READY;

uint32_t pulseStartMs = 0;
uint32_t lastAcceptedBlinkMs = 0;
bool hasAcceptedBlink = false;
float pulsePeakMagnitude = 0.0f;
uint32_t lastPulseDurationMs = 0;

uint8_t blinkCount = 0;
uint32_t sequenceStartMs = 0;
uint32_t lastSequenceBlinkMs = 0;

bool commandLocked = false;
uint32_t commandLockoutStartMs = 0;

uint32_t lastSampleUs = 0;
uint32_t lastDebugMs = 0;

// -----------------------------------------------------------------------------
// Telemetry & Event Structures for Production Firebase Streaming
// -----------------------------------------------------------------------------
struct TelemetryData {
  float filteredSignal;
  float sensorVoltageMv;
  uint8_t blinkCount;
  char systemState[8];
  char detectorState[16];
  char selectedOption[16];
};

struct CommandEvent {
  char command[32];
  uint8_t blinkCount;
  float peakMagnitude;
  uint32_t durationMs;
  char systemState[8];
  char selectedOption[16];
};

QueueHandle_t eventQueue = nullptr;
TelemetryData currentTelemetry = {0.0f, 0.0f, 0, "OFF", "READY", "Food"};
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

// Forward declarations
int readSensor();
float filterSignal(int rawValue);
bool detectBlink(float filteredValue, uint32_t nowMs);
void processBlinkSequence(uint32_t nowMs);
void executeCommand(uint8_t completedBlinkCount, uint32_t nowMs);

const char *systemStateName() {
  return currentState == SYSTEM_ON ? "ON" : "OFF";
}

const char *detectorStateName() {
  switch (detectorState) {
    case DETECTOR_READY: return "READY";
    case DETECTOR_TRACKING_PULSE: return "TRACKING";
    case DETECTOR_LONG_CLOSURE: return "LONG_CLOSURE";
    case DETECTOR_REFRACTORY: return "REFRACTORY";
  }
  return "UNKNOWN";
}

bool elapsed(uint32_t now, uint32_t since, uint32_t duration) {
  return static_cast<uint32_t>(now - since) >= duration;
}

float medianOfThree(float a, float b, float c) {
  if (a > b) {
    float temp = a;
    a = b;
    b = temp;
  }
  if (b > c) {
    float temp = b;
    b = c;
    c = temp;
  }
  if (a > b) {
    float temp = a;
    a = b;
    b = temp;
  }
  return b;
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    deviceConnected = true;
    Serial.println("BLE: client connected");
  }

  void onDisconnect(BLEServer *server) override {
    deviceConnected = false;
    advertisingRestartPending = true;
    Serial.println("BLE: client disconnected; advertising restart scheduled");
  }
};

class DataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    if (value.length() == 0) {
      return;
    }

    if (value.length() < 2) {
      Serial.printf("Web ACK: 1-byte value=0x%02X (expected tag+index)\n",
                    static_cast<uint8_t>(value[0]));
      return;
    }

    const uint8_t tag = static_cast<uint8_t>(value[0]);
    const uint8_t idx = static_cast<uint8_t>(value[1]);
    const bool idxValid = idx >= 1 && idx <= 6;

    switch (tag) {
      case 's':
        Serial.printf("Web ACK: menu highlight -> %s (index %u)\n",
                      idxValid ? OPTION_NAMES[idx - 1] : "?", idx);
        break;
      case 'a':
        Serial.printf("Web ACK: selected & spoke -> %s (index %u)\n",
                      idxValid ? OPTION_NAMES[idx - 1] : "?", idx);
        break;
      case 'x':
        Serial.printf("Web ACK: system %s\n", idx ? "ACTIVE" : "INACTIVE");
        break;
      default:
        Serial.printf("Web ACK: unrecognized tag=0x%02X idx=%u\n", tag, idx);
    }
  }
};

void sendBLEStatus(uint8_t status) {
  if (!deviceConnected) {
    return;
  }
  pCharacteristic->setValue(&status, 1);
  pCharacteristic->notify();
}

void sendBLEMenuAction(char action, uint8_t index) {
  if (!deviceConnected) {
    return;
  }
  uint8_t payload[2] = {static_cast<uint8_t>(action), index};
  pCharacteristic->setValue(payload, sizeof(payload));
  pCharacteristic->notify();
}

void resetBlinkSequence() {
  blinkCount = 0;
  sequenceStartMs = 0;
  lastSequenceBlinkMs = 0;
}

void handleSerialTuning() {
  if (!Serial.available()) {
    return;
  }
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) {
    return;
  }

  float newTrigger = blinkTriggerThreshold;
  float newRelease = blinkReleaseThreshold;
  bool changed = false;

  int tIndex = line.indexOf('T');
  int rIndex = line.indexOf('R');
  if (tIndex >= 0) {
    newTrigger = line.substring(tIndex + 1).toFloat();
    changed = true;
  }
  if (rIndex >= 0) {
    newRelease = line.substring(rIndex + 1).toFloat();
    changed = true;
  }

  if (!changed) {
    Serial.println("Usage: T<value> R<value>  e.g. T250 R120  (either or both)");
    return;
  }

  if (newRelease >= newTrigger) {
    Serial.println("Ignored: release threshold must be lower than trigger threshold");
    return;
  }

  blinkTriggerThreshold = newTrigger;
  blinkReleaseThreshold = newRelease;
  Serial.printf("Thresholds updated -> trigger=%.0f release=%.0f\n",
                blinkTriggerThreshold, blinkReleaseThreshold);
}

int readSensor() {
  return analogRead(SENSOR_PIN);
}

float filterSignal(int rawValue) {
  rawHistory[rawHistoryIndex] = static_cast<float>(rawValue);
  rawHistoryIndex = (rawHistoryIndex + 1) % 3;

  if (rawHistoryCount < 3) {
    ++rawHistoryCount;
    baseline = static_cast<float>(rawValue);
    return 0.0f;
  }

  const float medianValue = medianOfThree(rawHistory[0], rawHistory[1], rawHistory[2]);
  if (!filterReady) {
    baseline = medianValue;
    filteredSignal = 0.0f;
    filterReady = true;
    return filteredSignal;
  }

  baseline += BASELINE_ALPHA * (medianValue - baseline);
  const float dcRemoved = medianValue - baseline;
  filteredSignal += LOW_PASS_ALPHA * (dcRemoved - filteredSignal);
  return filteredSignal;
}

bool detectBlink(float filteredValue, uint32_t nowMs) {
  const float magnitude = fabsf(filteredValue);

  switch (detectorState) {
    case DETECTOR_READY:
      if (magnitude >= blinkTriggerThreshold) {
        pulseStartMs = nowMs;
        pulsePeakMagnitude = magnitude;
        detectorState = DETECTOR_TRACKING_PULSE;
      }
      break;

    case DETECTOR_TRACKING_PULSE:
      if (magnitude > pulsePeakMagnitude) {
        pulsePeakMagnitude = magnitude;
      }
      if (elapsed(nowMs, pulseStartMs, MAX_BLINK_DURATION_MS)) {
        detectorState = DETECTOR_LONG_CLOSURE;
        Serial.printf("Blink ignored: long closure | peak=%.0f\n", pulsePeakMagnitude);
      } else if (magnitude <= blinkReleaseThreshold) {
        const uint32_t pulseDuration = nowMs - pulseStartMs;
        lastPulseDurationMs = pulseDuration;
        detectorState = DETECTOR_REFRACTORY;

        const bool durationIsValid = pulseDuration >= MIN_BLINK_DURATION_MS;
        const bool debouncePassed = !hasAcceptedBlink ||
                                    elapsed(nowMs, lastAcceptedBlinkMs, BLINK_REFRACTORY_MS);

        Serial.printf("Pulse ended | peak=%.0f duration=%lums accepted=%s\n",
                      pulsePeakMagnitude, pulseDuration,
                      (durationIsValid && debouncePassed) ? "YES" : "NO");

        if (durationIsValid && debouncePassed) {
          lastAcceptedBlinkMs = nowMs;
          hasAcceptedBlink = true;
          return true;
        }

        if (!durationIsValid) {
          Serial.println("Blink ignored: pulse too short");
        }
      }
      break;

    case DETECTOR_LONG_CLOSURE:
      if (magnitude <= blinkReleaseThreshold) {
        detectorState = DETECTOR_REFRACTORY;
      }
      break;

    case DETECTOR_REFRACTORY:
      if (magnitude <= blinkReleaseThreshold &&
          (!hasAcceptedBlink || elapsed(nowMs, lastAcceptedBlinkMs, BLINK_REFRACTORY_MS))) {
        detectorState = DETECTOR_READY;
      }
      break;
  }

  return false;
}

void registerBlink(uint32_t nowMs) {
  if (commandLocked) {
    Serial.println("Blink ignored: command lockout active");
    return;
  }

  if (blinkCount > 0 &&
      (elapsed(nowMs, lastSequenceBlinkMs, INTER_BLINK_TIMEOUT_MS) ||
       elapsed(nowMs, sequenceStartMs, MAX_SEQUENCE_WINDOW_MS))) {
    processBlinkSequence(nowMs);
    if (commandLocked) {
      return;
    }
  }

  if (blinkCount == 0) {
    sequenceStartMs = nowMs;
  }

  ++blinkCount;
  lastSequenceBlinkMs = nowMs;
  Serial.printf("Blink detected | Blink count: %u | System state: %s\n",
                blinkCount, systemStateName());

  if (blinkCount == 4) {
    processBlinkSequence(nowMs);
  }
}

void processBlinkSequence(uint32_t nowMs) {
  if (blinkCount == 0) {
    return;
  }

  const uint8_t completedBlinkCount = blinkCount;
  resetBlinkSequence();
  executeCommand(completedBlinkCount, nowMs);
}

void executeCommand(uint8_t completedBlinkCount, uint32_t nowMs) {
  const char *executedCommand = "No command (unsupported blink count)";

  switch (completedBlinkCount) {
    case 1:
      if (currentState == SYSTEM_ON) {
        currentMenuIndex = (currentMenuIndex % MAX_MENU_ITEMS) + 1;
        sendBLEMenuAction('S', currentMenuIndex);
        executedCommand = "Rotate menu";
      } else {
        executedCommand = "Ignored: system inactive";
      }
      break;

    case 2:
      if (currentState == SYSTEM_ON) {
        sendBLEMenuAction('A', currentMenuIndex);
        executedCommand = "Speak selected command";
      } else {
        executedCommand = "Ignored: system inactive";
      }
      break;

    case 4:
      currentState = currentState == SYSTEM_ON ? SYSTEM_OFF : SYSTEM_ON;
      if (currentState == SYSTEM_ON) {
        currentMenuIndex = FIRST_MENU_ITEM;
        sendBLEStatus(BLE_SYSTEM_ACTIVATED);
        executedCommand = "System activated";
      } else {
        sendBLEStatus(BLE_SYSTEM_INACTIVE);
        executedCommand = "System inactive";
      }
      break;
  }

  Serial.printf("Executed command: %s | State: %s | Completed blinks: %u\n",
                executedCommand, systemStateName(), completedBlinkCount);

  // Queue command event to be pushed live to Firebase RTDB (/events)
  CommandEvent ev;
  strncpy(ev.command, executedCommand, sizeof(ev.command) - 1);
  ev.command[sizeof(ev.command) - 1] = '\0';
  ev.blinkCount = completedBlinkCount;
  ev.peakMagnitude = pulsePeakMagnitude;
  ev.durationMs = lastPulseDurationMs;
  strncpy(ev.systemState, systemStateName(), sizeof(ev.systemState) - 1);
  ev.systemState[sizeof(ev.systemState) - 1] = '\0';
  uint8_t optIdx = (currentMenuIndex >= 1 && currentMenuIndex <= 6) ? (currentMenuIndex - 1) : 0;
  strncpy(ev.selectedOption, OPTION_NAMES[optIdx], sizeof(ev.selectedOption) - 1);
  ev.selectedOption[sizeof(ev.selectedOption) - 1] = '\0';

  if (eventQueue != nullptr) {
    xQueueSend(eventQueue, &ev, 0);
  }

  commandLocked = true;
  commandLockoutStartMs = nowMs;
}

// -----------------------------------------------------------------------------
// FreeRTOS Task for Asynchronous Non-Blocking Firebase Streaming
// -----------------------------------------------------------------------------
void firebaseTask(void *pvParameters) {
  Serial.printf("Wi-Fi: Connecting to SSID '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected! IP address: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWi-Fi connection pending. Retrying in background...");
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  uint32_t lastLiveStreamMs = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));

    if (WiFi.status() != WL_CONNECTED) {
      if (WiFi.status() == WL_DISCONNECTED) {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // 1. Log gesture command events to Firebase (/events)
    CommandEvent ev;
    if (eventQueue != nullptr && xQueueReceive(eventQueue, &ev, 0) == pdTRUE) {
      String url = String("https://") + FIREBASE_HOST + "/events.json?auth=" + FIREBASE_API_KEY;
      if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        String payload = "{";
        payload += "\"command\":\"" + String(ev.command) + "\",";
        payload += "\"blinkCount\":" + String(ev.blinkCount) + ",";
        payload += "\"peakMagnitude\":" + String(ev.peakMagnitude, 2) + ",";
        payload += "\"durationMs\":" + String(ev.durationMs) + ",";
        payload += "\"systemState\":\"" + String(ev.systemState) + "\",";
        payload += "\"selectedOption\":\"" + String(ev.selectedOption) + "\",";
        payload += "\"timestamp\":" + String(millis());
        payload += "}";
        int httpCode = http.POST(payload);
        if (httpCode > 0) {
          Serial.printf("Firebase: Event logged live (HTTP %d)\n", httpCode);
        } else {
          Serial.printf("Firebase: Event log error (%s)\n", http.errorToString(httpCode).c_str());
        }
        http.end();
      }
    }

    // 2. Stream live sensor telemetry to Firebase (/live_data) every 200ms
    uint32_t now = millis();
    if (now - lastLiveStreamMs >= 200) {
      lastLiveStreamMs = now;

      TelemetryData dataCopy;
      portENTER_CRITICAL(&telemetryMux);
      dataCopy = currentTelemetry;
      portEXIT_CRITICAL(&telemetryMux);

      String url = String("https://") + FIREBASE_HOST + "/live_data.json?auth=" + FIREBASE_API_KEY;
      if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        String payload = "{";
        payload += "\"filteredSignal\":" + String(dataCopy.filteredSignal, 2) + ",";
        payload += "\"sensorVoltageMv\":" + String(dataCopy.sensorVoltageMv, 2) + ",";
        payload += "\"blinkCount\":" + String(dataCopy.blinkCount) + ",";
        payload += "\"systemState\":\"" + String(dataCopy.systemState) + "\",";
        payload += "\"detectorState\":\"" + String(dataCopy.detectorState) + "\",";
        payload += "\"selectedOption\":\"" + String(dataCopy.selectedOption) + "\",";
        payload += "\"timestamp\":" + String(now);
        payload += "}";
        http.PUT(payload);
        http.end();
      }
    }
  }
}

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

  // Create event queue and spawn Firebase task on FreeRTOS core
  eventQueue = xQueueCreate(10, sizeof(CommandEvent));
  xTaskCreatePinnedToCore(
      firebaseTask,
      "FirebaseTask",
      8192,
      nullptr,
      1,
      nullptr,
      1
  );

  Serial.println("NeuroSpeak ready: robust blink FSM & Firebase streaming enabled");
  Serial.printf("Trigger=%.0f Release=%.0f Min=%lums Max=%lums Window=%lums\n",
                blinkTriggerThreshold, blinkReleaseThreshold,
                MIN_BLINK_DURATION_MS, MAX_BLINK_DURATION_MS,
                MAX_SEQUENCE_WINDOW_MS);
}

void loop() {
  const uint32_t nowUs = micros();
  const uint32_t nowMs = millis();

  handleSerialTuning();

  if (advertisingRestartPending) {
    advertisingRestartPending = false;
    if (pAdvertising != nullptr) {
      pAdvertising->start();
      Serial.println("BLE: advertising restarted");
    }
  }

  if (commandLocked && elapsed(nowMs, commandLockoutStartMs, COMMAND_LOCKOUT_MS)) {
    commandLocked = false;
  }

  // Fixed-rate sampling keeps filter behavior stable without blocking the loop.
  if (static_cast<uint32_t>(nowUs - lastSampleUs) >= SAMPLE_INTERVAL_US) {
    lastSampleUs = nowUs;
    const int rawValue = readSensor();
    const float filteredValue = filterSignal(rawValue);
    const float voltageMv = static_cast<float>(analogReadMilliVolts(SENSOR_PIN));
    const bool blinkDetected = filterReady && detectBlink(filteredValue, nowMs);

    if (blinkDetected) {
      registerBlink(nowMs);
    }

    // Thread-safe update of current production telemetry
    portENTER_CRITICAL(&telemetryMux);
    currentTelemetry.filteredSignal = filteredValue;
    currentTelemetry.sensorVoltageMv = voltageMv;
    currentTelemetry.blinkCount = blinkCount;
    strncpy(currentTelemetry.systemState, systemStateName(), sizeof(currentTelemetry.systemState) - 1);
    currentTelemetry.systemState[sizeof(currentTelemetry.systemState) - 1] = '\0';
    strncpy(currentTelemetry.detectorState, detectorStateName(), sizeof(currentTelemetry.detectorState) - 1);
    currentTelemetry.detectorState[sizeof(currentTelemetry.detectorState) - 1] = '\0';
    uint8_t idx = (currentMenuIndex >= 1 && currentMenuIndex <= 6) ? (currentMenuIndex - 1) : 0;
    strncpy(currentTelemetry.selectedOption, OPTION_NAMES[idx], sizeof(currentTelemetry.selectedOption) - 1);
    currentTelemetry.selectedOption[sizeof(currentTelemetry.selectedOption) - 1] = '\0';
    portEXIT_CRITICAL(&telemetryMux);

    if (elapsed(nowMs, lastDebugMs, DEBUG_INTERVAL_MS)) {
      lastDebugMs = nowMs;
      Serial.printf("Filtered:%.1f Voltage:%.1fmV BlinkDetected:%s BlinkCount:%u State:%s Detector:%s\n",
                    filteredValue, voltageMv, blinkDetected ? "YES" : "NO", blinkCount,
                    systemStateName(), detectorStateName());
    }
  }

  // Finalize sequence
  if (blinkCount > 0 &&
      (elapsed(nowMs, lastSequenceBlinkMs, INTER_BLINK_TIMEOUT_MS) ||
       elapsed(nowMs, sequenceStartMs, MAX_SEQUENCE_WINDOW_MS))) {
    processBlinkSequence(nowMs);
  }
}