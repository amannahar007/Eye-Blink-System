/*
 * NeuroSpeak - High-Precision Biomedical EOG Eye-Blink Assistive Controller
 * Hardware: ESP32-C6 Microcontroller + Upside Down Labs BioAmp EXG Pill
 * 
 * Custom Control Sequence:
 *  - 4 Blinks: Activate System (Sends 1-byte BLE '0')
 *  - 1 Blink:  Next Menu Item (Sends 2-byte BLE 'S' + Index)
 *  - 2 Blinks: Select Item    (Sends 2-byte BLE 'A' + Index)
 *  - 4 Blinks or 10s Timeout: Deactivate (Sends 1-byte BLE '127')
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <math.h>

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
constexpr uint32_t SAMPLE_RATE = 512;           // MUST be 512 for UDL Filters
constexpr uint32_t SAMPLE_INTERVAL_US = 1953;   // 1000000 / 512 Hz

const char *OPTION_NAMES[6] = {
    "Food", "Help", "Outing", "Television", "Washroom", "Water"
};

// -----------------------------------------------------------------------------
// UDL Signal Processing Buffers (512Hz Tuned)
// -----------------------------------------------------------------------------
#define ENVELOPE_WINDOW_MS 100 
#define ENVELOPE_WINDOW_SIZE ((ENVELOPE_WINDOW_MS * SAMPLE_RATE) / 1000)

float envelopeBuffer[ENVELOPE_WINDOW_SIZE] = {0};
int envelopeIndex = 0;
float envelopeSum = 0;
float currentEEGEnvelope = 0;
float BlinkThreshold = 50.0;

// -----------------------------------------------------------------------------
// Sequence Classifier Variables
// -----------------------------------------------------------------------------
const uint32_t BLINK_DEBOUNCE_MS = 250;       // Minimum time between physical blinks
const uint32_t SEQUENCE_TIMEOUT_MS = 1000;    // Time to wait after last blink to lock in command
const uint32_t MENU_TIMEOUT_MS = 10000;       // 10 seconds of inactivity turns off system

uint32_t lastBlinkTime = 0;
uint32_t lastActivityTime = 0;
int currentBlinkSequenceCount = 0;

// -----------------------------------------------------------------------------
// BLE Global Controls & State
// -----------------------------------------------------------------------------
BLEServer *pServer = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
BLEAdvertising *pAdvertising = nullptr;
bool deviceConnected = false;
volatile bool advertisingRestartPending = false;

enum SystemState : uint8_t { SYSTEM_OFF, SYSTEM_ON };
SystemState currentState = SYSTEM_OFF;

constexpr uint8_t FIRST_MENU_ITEM = 1;
constexpr uint8_t MAX_MENU_ITEMS = 6;
uint8_t currentMenuIndex = FIRST_MENU_ITEM;

uint32_t lastSampleUs = 0;

struct TelemetryData {
  uint8_t blinkCount;
  bool systemActivated;
  char selectedOutput[16];
};
TelemetryData currentTelemetry = {0, false, "Food"};
portMUX_TYPE telemetryMux = portMUX_INITIALIZER_UNLOCKED;

// -----------------------------------------------------------------------------
// UDL Digital Filters (Generated via filter_gen.py @ 512Hz)
// -----------------------------------------------------------------------------
float highpass(float input) {
  float output = input;
  {
    static float z1, z2;
    float x = output - -1.91327599*z1 - 0.91688335*z2;
    output = 0.95753983*x + -1.91507967*z1 + 0.95753983*z2;
    z2 = z1; z1 = x;
  }
  return output;
}

float Notch(float input) {
  float output = input;
  {
    static float z1, z2;
    float x = output - -1.58696045*z1 - 0.96505858*z2;
    output = 0.96588529*x + -1.57986211*z1 + 0.96588529*z2;
    z2 = z1; z1 = x;
  }
  {
    static float z1, z2;
    float x = output - -1.62761184*z1 - 0.96671306*z2;
    output = 1.00000000*x + -1.63566226*z1 + 1.00000000*z2;
    z2 = z1; z1 = x;
  }
  return output;
}

float EEGFilter(float input) {
  float output = input;
  {
    static float z1, z2;
    float x = output - -1.24200128*z1 - 0.45885207*z2;
    output = 0.05421270*x + 0.10842539*z1 + 0.05421270*z2;
    z2 = z1; z1 = x;
  }
  return output;
}

float updateEEGEnvelope(float sample) {
  float absSample = fabs(sample);
  envelopeSum -= envelopeBuffer[envelopeIndex];
  envelopeSum += absSample;
  envelopeBuffer[envelopeIndex] = absSample;
  envelopeIndex = (envelopeIndex + 1) % ENVELOPE_WINDOW_SIZE;
  return envelopeSum / ENVELOPE_WINDOW_SIZE;
}

// -----------------------------------------------------------------------------
// BLE Callbacks & Transmitters
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
    String value = characteristic->getValue(); // FIXED: Changed to Arduino String
    if (value.length() >= 2) {
      char tag = value[0];
      uint8_t idx = value[1];
      // Prints acknowledgment from the React Web App to verify closed loop
      Serial.printf("Web ACK Received -> Tag: %c, Index: %u\n", tag, idx);
    }
  }
};

void sendBLEMenuAction(char action, uint8_t index) {
  if (!deviceConnected) return;
  
  if (action == 'X') {
    // Send exactly 1 byte for System ON (0) or OFF (127) - Matches Mainpage.tsx logic[cite: 1]
    uint8_t payload[1] = {index};
    pCharacteristic->setValue(payload, 1);
  } else {
    // Send exactly 2 bytes for Navigate ('S') and Select ('A') - Matches Mainpage.tsx logic[cite: 1]
    uint8_t payload[2] = {static_cast<uint8_t>(action), index};
    pCharacteristic->setValue(payload, 2);
  }
  pCharacteristic->notify();
}

// -----------------------------------------------------------------------------
// Telemetry & Firebase Task
// -----------------------------------------------------------------------------
void updateTelemetry(uint8_t blinkCount) {
    portENTER_CRITICAL(&telemetryMux);
    currentTelemetry.blinkCount = blinkCount;
    currentTelemetry.systemActivated = (currentState == SYSTEM_ON);
    uint8_t idx = (currentMenuIndex >= 1 && currentMenuIndex <= 6) ? (currentMenuIndex - 1) : 0;
    strncpy(currentTelemetry.selectedOutput, OPTION_NAMES[idx], sizeof(currentTelemetry.selectedOutput) - 1);
    currentTelemetry.selectedOutput[sizeof(currentTelemetry.selectedOutput) - 1] = '\0';
    portEXIT_CRITICAL(&telemetryMux);
}

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
  Serial.printf("Wi-Fi: Connecting to SSID '%s'...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  Serial.println("\nWi-Fi connected!");

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
// Arduino Setup & Loop
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
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  xTaskCreatePinnedToCore(firebaseTask, "FirebaseTask", 8192, nullptr, 1, nullptr, 0);

  lastSampleUs = micros();
  Serial.println("NeuroSpeak Biomedical EOG Controller Ready.");
}

void loop() {
  uint32_t nowUs = micros();
  uint32_t nowMs = millis();

  if (advertisingRestartPending) {
    advertisingRestartPending = false;
    if (pAdvertising != nullptr) pAdvertising->start();
  }

  // 1. Process Signal strictly at 512 Hz
  if (static_cast<uint32_t>(nowUs - lastSampleUs) >= SAMPLE_INTERVAL_US) {
    lastSampleUs += SAMPLE_INTERVAL_US;
    
    int raw = analogRead(SENSOR_PIN);
    float filt = EEGFilter(Notch(raw));
    float filtered = highpass(filt);
    currentEEGEnvelope = updateEEGEnvelope(filtered);

    // 2. Detect Blink with Debounce
    if (currentEEGEnvelope > BlinkThreshold && (nowMs - lastBlinkTime) >= BLINK_DEBOUNCE_MS) {
        lastBlinkTime = nowMs;
        lastActivityTime = nowMs; // Reset inactivity timer
        currentBlinkSequenceCount++;
        Serial.printf("Blink Logged! Sequence Count: %d\n", currentBlinkSequenceCount);
    }
  }

  // 3. Evaluate Sequence Logic (Triggered after 1s of no blinking)
  if (currentBlinkSequenceCount > 0 && (nowMs - lastBlinkTime) > SEQUENCE_TIMEOUT_MS) {
      Serial.printf(">>> Sequence Executing: %d blinks <<<\n", currentBlinkSequenceCount);

      if (currentBlinkSequenceCount == 4) {
          // Toggle System Status
          currentState = (currentState == SYSTEM_OFF) ? SYSTEM_ON : SYSTEM_OFF;
          sendBLEMenuAction('X', currentState == SYSTEM_ON ? BLE_SYSTEM_ACTIVATED : BLE_SYSTEM_INACTIVE);
          Serial.println(currentState == SYSTEM_ON ? "System Activated (ON)" : "System Deactivated (OFF)");
          
          if (currentState == SYSTEM_ON) {
              currentMenuIndex = FIRST_MENU_ITEM; // Start at index 1
          }
      } 
      else if (currentState == SYSTEM_ON) {
          if (currentBlinkSequenceCount == 1) {
              // Menu Rotation
              currentMenuIndex = (currentMenuIndex % MAX_MENU_ITEMS) + 1;
              sendBLEMenuAction('S', currentMenuIndex);
              Serial.printf("Menu Navigated -> %s\n", OPTION_NAMES[currentMenuIndex - 1]);
          } 
          else if (currentBlinkSequenceCount == 2) {
              // Command Select
              sendBLEMenuAction('A', currentMenuIndex);
              Serial.printf("Command Triggered -> %s\n", OPTION_NAMES[currentMenuIndex - 1]);
          }
      } else {
          Serial.println("Ignored: System is currently OFF.");
      }

      updateTelemetry(currentBlinkSequenceCount);
      currentBlinkSequenceCount = 0; // Reset for next sequence
  }

  // 4. Handle 10-Second Inactivity Timeout
  if (currentState == SYSTEM_ON && (nowMs - lastActivityTime) > MENU_TIMEOUT_MS) {
      Serial.println("10s Inactivity Timeout -> System Deactivated");
      currentState = SYSTEM_OFF;
      sendBLEMenuAction('X', BLE_SYSTEM_INACTIVE); 
      updateTelemetry(0);
      lastActivityTime = nowMs; // Prevent loop spam
  }
}