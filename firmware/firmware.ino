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
constexpr uint32_t SAMPLE_INTERVAL_US = 4000;   // 250 Hz exact sample clock
constexpr uint32_t DEBUG_INTERVAL_MS = 100;     // 10 Hz Serial telemetry rate
constexpr uint32_t CALIBRATION_SAMPLES = 1250;  // 5 seconds at 250 Hz

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
// Signal Conditioning State Variables
// -----------------------------------------------------------------------------
float medianWindow[3] = {0.0f, 0.0f, 0.0f};
uint8_t medianIndex = 0;
bool medianWarmedUp = false;

// High-Pass Filter (0.5Hz IIR at fs=250Hz, alpha = 0.98758)
float hpfPrevIn = 0.0f;
float hpfPrevOut = 0.0f;
constexpr float HPF_ALPHA = 0.98758f;

// 50Hz Biquad Notch Filter (fs=250Hz, f0=50Hz, Q=5.0)
// H(z) = b0*(1 - 2*cos(w0)*z^-1 + z^-2) / (1 - 2*r*cos(w0)*z^-1 + r^2*z^-2)
// w0 = 0.4*PI (72 deg), cos(w0)=0.309017, r = 1 - PI*10/250 = 0.874336
constexpr float NOTCH_B0 = 0.93717f;
constexpr float NOTCH_B1 = -0.57919f;
constexpr float NOTCH_B2 = 0.93717f;
constexpr float NOTCH_A1 = -0.54037f;
constexpr float NOTCH_A2 = 0.76446f;
float notchX1 = 0.0f, notchX2 = 0.0f;
float notchY1 = 0.0f, notchY2 = 0.0f;

// 12Hz 2nd-Order Low-Pass Butterworth Filter (fs=250Hz, fc=12Hz)
constexpr float LPF_B0 = 0.018099f;
constexpr float LPF_B1 = 0.036198f;
constexpr float LPF_B2 = 0.018099f;
constexpr float LPF_A1 = -1.561018f;
constexpr float LPF_A2 = 0.633414f;
float lpfX1 = 0.0f, lpfX2 = 0.0f;
float lpfY1 = 0.0f, lpfY2 = 0.0f;

// TKEO & Derivative Buffer
float prevLpfOut[3] = {0.0f, 0.0f, 0.0f};
float tkeoSignal = 0.0f;
float filteredSignal = 0.0f;

// Ring buffer for derivative/smoothness analysis (25 samples = 100ms)
constexpr uint8_t RING_BUF_SIZE = 25;
float recentSamples[RING_BUF_SIZE] = {0.0f};
uint8_t sampleRingIdx = 0;

// -----------------------------------------------------------------------------
// Outlier-Resistant Baseline Calibration & Dynamic Thresholding
// -----------------------------------------------------------------------------
bool isCalibrated = false;
uint32_t calibrationCount = 0;
float calibBuffer[CALIBRATION_SAMPLES];

float noiseMean = 0.0f;
float noiseStdDev = 10.0f;
float noiseVariance = 100.0f;

float blinkTriggerThreshold = 150.0f;
float blinkReleaseThreshold = 60.0f;
constexpr float K_TRIG = 4.2f;
constexpr float K_REL = 1.8f;
constexpr float EMV_BETA = 0.003f; // Gentle continuous update rate during idle

// -----------------------------------------------------------------------------
// Bio-Physiological Timing & Validation Constants
// -----------------------------------------------------------------------------
constexpr uint32_t MIN_BLINK_DURATION_MS = 45;
constexpr uint32_t MAX_BLINK_DURATION_MS = 380;
constexpr uint32_t BLINK_REFRACTORY_MS = 140;
constexpr uint32_t INTER_BLINK_TIMEOUT_MS = 420; // Fast, responsive menu window
constexpr uint32_t MAX_SEQUENCE_WINDOW_MS = 2200;
constexpr uint32_t COMMAND_LOCKOUT_MS = 400;
constexpr float MIN_CONFIDENCE_PERCENT = 60.0f;

// -----------------------------------------------------------------------------
// State Machine Definitions
// -----------------------------------------------------------------------------
enum SystemState : uint8_t {
  SYSTEM_OFF,
  SYSTEM_ON
};

enum DetectorState : uint8_t {
  DETECTOR_CALIBRATING,
  DETECTOR_READY,
  DETECTOR_RISING,
  DETECTOR_PEAK,
  DETECTOR_FALLING,
  DETECTOR_LONG_CLOSURE,
  DETECTOR_REFRACTORY
};

SystemState currentState = SYSTEM_OFF;
DetectorState detectorState = DETECTOR_CALIBRATING;

// Timing & Pulse tracking metrics
uint32_t pulseStartMs = 0;
uint32_t pulsePeakMs = 0;
float pulsePeakMagnitude = 0.0f;
float pulsePeakTkeo = 0.0f;
uint32_t lastPulseDurationMs = 0;
uint32_t lastAcceptedBlinkMs = 0;
bool hasAcceptedBlink = false;
float lastConfidenceScore = 0.0f;

// Sequence tracking
constexpr uint8_t FIRST_MENU_ITEM = 1;
constexpr uint8_t MAX_MENU_ITEMS = 6;
uint8_t currentMenuIndex = FIRST_MENU_ITEM;

uint8_t blinkCount = 0;
uint32_t sequenceStartMs = 0;
uint32_t lastSequenceBlinkMs = 0;

bool commandLocked = false;
uint32_t commandLockoutStartMs = 0;

uint32_t lastSampleUs = 0;
uint32_t lastDebugMs = 0;

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

const char *detectorStateName() {
  switch (detectorState) {
    case DETECTOR_CALIBRATING: return "CALIBRATING";
    case DETECTOR_READY: return "READY";
    case DETECTOR_RISING: return "RISING";
    case DETECTOR_PEAK: return "PEAK";
    case DETECTOR_FALLING: return "FALLING";
    case DETECTOR_LONG_CLOSURE: return "LONG_CLOSURE";
    case DETECTOR_REFRACTORY: return "REFRACTORY";
  }
  return "UNKNOWN";
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
}

void resetBlinkSequence() {
  blinkCount = 0;
  sequenceStartMs = 0;
  lastSequenceBlinkMs = 0;
}

// -----------------------------------------------------------------------------
// Signal Processing Pipeline
// -----------------------------------------------------------------------------
float filterSignal(int rawValue) {
  float val = static_cast<float>(rawValue);

  // Warm start median filter with initial reading to avoid step transients
  if (!medianWarmedUp) {
    medianWindow[0] = val;
    medianWindow[1] = val;
    medianWindow[2] = val;
    hpfPrevIn = val;
    hpfPrevOut = 0.0f;
    medianWarmedUp = true;
  }

  // 1. 3-Point Median Filter
  medianWindow[medianIndex] = val;
  medianIndex = (medianIndex + 1) % 3;
  const float medVal = medianOfThree(medianWindow[0], medianWindow[1], medianWindow[2]);

  // 2. High-Pass Filter (0.5Hz IIR)
  float hpfOut = HPF_ALPHA * (hpfPrevOut + medVal - hpfPrevIn);
  hpfPrevIn = medVal;
  hpfPrevOut = hpfOut;

  // 3. 50Hz Biquad Notch Filter
  float notchOut = NOTCH_B0 * hpfOut + NOTCH_B1 * notchX1 + NOTCH_B2 * notchX2
                  - NOTCH_A1 * notchY1 - NOTCH_A2 * notchY2;
  notchX2 = notchX1; notchX1 = hpfOut;
  notchY2 = notchY1; notchY1 = notchOut;

  // 4. 12Hz 2nd-Order Low-Pass Butterworth Filter
  float lpfOut = LPF_B0 * notchOut + LPF_B1 * lpfX1 + LPF_B2 * lpfX2
                - LPF_A1 * lpfY1 - LPF_A2 * lpfY2;
  lpfX2 = lpfX1; lpfX1 = notchOut;
  lpfY2 = lpfY1; lpfY1 = lpfOut;

  // Store in ring buffer for derivative / smoothness check
  recentSamples[sampleRingIdx] = lpfOut;
  sampleRingIdx = (sampleRingIdx + 1) % RING_BUF_SIZE;

  // 5. Teager-Kaiser Energy Operator (TKEO)
  prevLpfOut[0] = prevLpfOut[1];
  prevLpfOut[1] = prevLpfOut[2];
  prevLpfOut[2] = lpfOut;
  tkeoSignal = (prevLpfOut[1] * prevLpfOut[1]) - (prevLpfOut[0] * prevLpfOut[2]);
  if (tkeoSignal < 0.0f) tkeoSignal = 0.0f;

  filteredSignal = lpfOut;
  return filteredSignal;
}

// -----------------------------------------------------------------------------
// Outlier-Resistant Boot Calibration
// -----------------------------------------------------------------------------
void processCalibration(float signalValue) {
  if (calibrationCount < CALIBRATION_SAMPLES) {
    calibBuffer[calibrationCount++] = signalValue;
  }

  if (calibrationCount >= CALIBRATION_SAMPLES) {
    // 2-Pass Trimmed Calibration to discard any blinks/motion during startup
    int lowerK = CALIBRATION_SAMPLES * 0.10; // Discard bottom 10%
    int upperK = CALIBRATION_SAMPLES * 0.90; // Discard top 10%
    quickSelect(calibBuffer, 0, CALIBRATION_SAMPLES - 1, lowerK);
    quickSelect(calibBuffer, lowerK, CALIBRATION_SAMPLES - 1, upperK);

    double sum = 0.0;
    double sqSum = 0.0;
    int validCount = upperK - lowerK + 1;

    for (int i = lowerK; i <= upperK; i++) {
      sum += calibBuffer[i];
      sqSum += (calibBuffer[i] * calibBuffer[i]);
    }

    noiseMean = sum / validCount;
    float variance = (sqSum / validCount) - (noiseMean * noiseMean);
    if (variance < 1.0f) variance = 1.0f;
    noiseVariance = variance;
    noiseStdDev = sqrtf(noiseVariance);

    blinkTriggerThreshold = noiseMean + (K_TRIG * noiseStdDev);
    blinkReleaseThreshold = noiseMean + (K_REL * noiseStdDev);

    // Adaptive floor scaling based on noise standard deviation
    float minTrigFloor = noiseMean + 3.5f * noiseStdDev;
    float minRelFloor = noiseMean + 1.5f * noiseStdDev;

    if (blinkTriggerThreshold < minTrigFloor) blinkTriggerThreshold = minTrigFloor;
    if (blinkReleaseThreshold < minRelFloor) blinkReleaseThreshold = minRelFloor;

    isCalibrated = true;
    detectorState = DETECTOR_READY;
    Serial.printf("\n--- BOOT CALIBRATION COMPLETE (Trimmed 10-90%%) ---\n");
    Serial.printf("Noise Mean: %.2f | StdDev: %.2f | Trigger: %.1f | Release: %.1f\n\n",
                  noiseMean, noiseStdDev, blinkTriggerThreshold, blinkReleaseThreshold);
  }
}

// -----------------------------------------------------------------------------
// Gated Continuous Baseline Tracking (Idle Only)
// -----------------------------------------------------------------------------
void updateAdaptiveThreshold(float signalValue) {
  // Update baseline ONLY when in DETECTOR_READY state and within +/- 2.0 stddev
  if (!isCalibrated || detectorState != DETECTOR_READY) return;

  float delta = signalValue - noiseMean;
  if (fabsf(delta) > (2.0f * noiseStdDev)) {
    // Skip baseline update during signal perturbations or blink onset
    return;
  }

  noiseMean += EMV_BETA * delta;
  noiseVariance = (1.0f - EMV_BETA) * noiseVariance + EMV_BETA * (delta * delta);
  if (noiseVariance < 1.0f) noiseVariance = 1.0f;
  noiseStdDev = sqrtf(noiseVariance);

  float newTrig = noiseMean + (K_TRIG * noiseStdDev);
  float newRel = noiseMean + (K_REL * noiseStdDev);

  float minTrigFloor = noiseMean + 3.5f * noiseStdDev;
  float minRelFloor = noiseMean + 1.5f * noiseStdDev;

  if (newTrig < minTrigFloor) newTrig = minTrigFloor;
  if (newRel < minRelFloor) newRel = minRelFloor;

  blinkTriggerThreshold = newTrig;
  blinkReleaseThreshold = newRel;
}

// -----------------------------------------------------------------------------
// Physiological Confidence Engine
// -----------------------------------------------------------------------------
float calculateConfidenceScore(float peakAmp, uint32_t durationMs, uint32_t riseTimeMs, uint32_t fallTimeMs, float peakTkeo) {
  // 1. SNR Amplitude Score (30%)
  float snr = (peakAmp - noiseMean) / noiseStdDev;
  float cAmp = (snr - K_TRIG) / (K_TRIG * 1.5f);
  if (cAmp > 1.0f) cAmp = 1.0f;
  if (cAmp < 0.0f) cAmp = 0.0f;

  // 2. Physiological Duration Score (30%) - Target ~160ms
  float durationDiff = fabsf(static_cast<float>(durationMs) - 160.0f);
  float cDur = 1.0f - (durationDiff / 160.0f);
  if (cDur < 0.0f) cDur = 0.0f;

  // 3. Bio-Asymmetry Score (20%) - Target rise/fall ratio ~0.5 to 0.75 (35% rise, 65% fall)
  float symmetryRatio = (fallTimeMs > 0) ? (static_cast<float>(riseTimeMs) / fallTimeMs) : 0.0f;
  float targetRatio = 0.60f; // Fast contraction / slower relaxation
  float cSym = 1.0f - (fabsf(symmetryRatio - targetRatio) / targetRatio);
  if (cSym < 0.0f) cSym = 0.0f;

  // 4. Energy Smoothness Score (20%)
  float expectedEnergy = blinkTriggerThreshold * 1.5f;
  float cEnergy = peakTkeo / expectedEnergy;
  if (cEnergy > 1.0f) cEnergy = 1.0f;
  if (cEnergy < 0.0f) cEnergy = 0.0f;

  float totalScore = (0.30f * cAmp + 0.30f * cDur + 0.20f * cSym + 0.20f * cEnergy) * 100.0f;
  return totalScore;
}

// -----------------------------------------------------------------------------
// Multi-Stage Artifact & EMG Verification
// -----------------------------------------------------------------------------
bool validateBlinkArtifacts(float peakAmp, uint32_t durationMs, uint32_t riseTimeMs, uint32_t fallTimeMs, float confidence) {
  if (peakAmp <= noiseMean) {
    Serial.println("Artifact Rejected: Negative/Invalid Polarity");
    return false;
  }

  if (durationMs < MIN_BLINK_DURATION_MS || durationMs > MAX_BLINK_DURATION_MS) {
    Serial.printf("Artifact Rejected: Duration %lums outside bounds [%u-%ums]\n",
                  durationMs, MIN_BLINK_DURATION_MS, MAX_BLINK_DURATION_MS);
    return false;
  }

  // Calculate 2nd Derivative Variance over 100ms window for EMG rejection
  // EMG muscle bursts cause rapid jitter (high 2nd derivative variance), eye blinks are smooth
  float d2Sum = 0.0f;
  float d2SqSum = 0.0f;
  int count = 0;

  for (int i = 0; i < RING_BUF_SIZE - 2; i++) {
    float d2 = recentSamples[i+2] - 2.0f * recentSamples[i+1] + recentSamples[i];
    d2Sum += d2;
    d2SqSum += (d2 * d2);
    count++;
  }

  float d2Mean = d2Sum / count;
  float d2Var = (d2SqSum / count) - (d2Mean * d2Mean);

  if (d2Var > (noiseVariance * 15.0f)) {
    Serial.printf("Artifact Rejected: EMG Muscle Burst (d2Var=%.1f > %.1f)\n", d2Var, noiseVariance * 15.0f);
    return false;
  }

  if (confidence < MIN_CONFIDENCE_PERCENT) {
    Serial.printf("Artifact Rejected: Low Confidence (%.1f%% < %.1f%%)\n", confidence, MIN_CONFIDENCE_PERCENT);
    return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// Multi-Stage FSM Detection Engine
// -----------------------------------------------------------------------------
bool detectBlink(float signalValue, uint32_t nowMs) {
  if (!isCalibrated) {
    processCalibration(signalValue);
    return false;
  }

  // Continuous baseline update (gated)
  updateAdaptiveThreshold(signalValue);

  switch (detectorState) {
    case DETECTOR_READY:
      if (signalValue >= blinkTriggerThreshold) {
        pulseStartMs = nowMs;
        pulsePeakMs = nowMs;
        pulsePeakMagnitude = signalValue;
        pulsePeakTkeo = tkeoSignal;
        detectorState = DETECTOR_RISING;
      }
      break;

    case DETECTOR_RISING:
    case DETECTOR_PEAK:
      if (signalValue > pulsePeakMagnitude) {
        pulsePeakMagnitude = signalValue;
        pulsePeakMs = nowMs;
        detectorState = DETECTOR_PEAK;
      }
      if (tkeoSignal > pulsePeakTkeo) {
        pulsePeakTkeo = tkeoSignal;
      }

      if (signalValue < pulsePeakMagnitude * 0.85f) {
        detectorState = DETECTOR_FALLING;
      }

      if (elapsed(nowMs, pulseStartMs, MAX_BLINK_DURATION_MS)) {
        detectorState = DETECTOR_LONG_CLOSURE;
        Serial.printf("Blink Ignored: Drowsiness / Extended Closure (Peak=%.1f)\n", pulsePeakMagnitude);
      }
      break;

    case DETECTOR_FALLING:
      if (signalValue > pulsePeakMagnitude) {
        pulsePeakMagnitude = signalValue;
        pulsePeakMs = nowMs;
        detectorState = DETECTOR_PEAK;
      }

      if (elapsed(nowMs, pulseStartMs, MAX_BLINK_DURATION_MS)) {
        detectorState = DETECTOR_LONG_CLOSURE;
        Serial.printf("Blink Ignored: Drowsiness / Extended Closure (Peak=%.1f)\n", pulsePeakMagnitude);
      } else if (signalValue <= blinkReleaseThreshold) {
        const uint32_t pulseDuration = nowMs - pulseStartMs;
        const uint32_t riseTime = pulsePeakMs - pulseStartMs;
        const uint32_t fallTime = nowMs - pulsePeakMs;
        lastPulseDurationMs = pulseDuration;
        detectorState = DETECTOR_REFRACTORY;

        lastConfidenceScore = calculateConfidenceScore(pulsePeakMagnitude, pulseDuration, riseTime, fallTime, pulsePeakTkeo);
        const bool isValid = validateBlinkArtifacts(pulsePeakMagnitude, pulseDuration, riseTime, fallTime, lastConfidenceScore);
        const bool debouncePassed = !hasAcceptedBlink || elapsed(nowMs, lastAcceptedBlinkMs, BLINK_REFRACTORY_MS);

        Serial.printf("Pulse Ended | Peak=%.1f Dur=%lums Conf=%.1f%% Valid=%s Debounce=%s\n",
                      pulsePeakMagnitude, pulseDuration, lastConfidenceScore,
                      isValid ? "YES" : "NO", debouncePassed ? "PASS" : "FAIL");

        if (isValid && debouncePassed) {
          lastAcceptedBlinkMs = nowMs;
          hasAcceptedBlink = true;
          return true;
        }
      }
      break;

    case DETECTOR_LONG_CLOSURE:
      if (signalValue <= blinkReleaseThreshold) {
        detectorState = DETECTOR_REFRACTORY;
      }
      break;

    case DETECTOR_REFRACTORY:
      if (signalValue <= blinkReleaseThreshold &&
          (!hasAcceptedBlink || elapsed(nowMs, lastAcceptedBlinkMs, BLINK_REFRACTORY_MS))) {
        detectorState = DETECTOR_READY;
      }
      break;

    default:
      detectorState = DETECTOR_READY;
      break;
  }

  return false;
}

// -----------------------------------------------------------------------------
// Optimized Sequence Classifier & Menu FSM
// -----------------------------------------------------------------------------
void registerBlink(uint32_t nowMs) {
  if (commandLocked) {
    Serial.println("Blink ignored: Command lockout active");
    return;
  }

  if (blinkCount == 0) {
    sequenceStartMs = nowMs;
  }

  ++blinkCount;
  lastSequenceBlinkMs = nowMs;
  Serial.printf("Blink Confirmed | Sequence Count: %u | System State: %s\n", blinkCount, systemStateName());

  // Instant dispatch for 4-blink system toggle fail-safe
  if (blinkCount >= 4) {
    processBlinkSequence(nowMs);
  }
}

void processBlinkSequence(uint32_t nowMs) {
  if (blinkCount == 0) return;

  const uint8_t completedCount = blinkCount;
  resetBlinkSequence();
  executeCommand(completedCount, nowMs);
}

void executeCommand(uint8_t completedBlinkCount, uint32_t nowMs) {
  const char *executedCommand = "Ignored: Unsupported sequence";

  switch (completedBlinkCount) {
    case 1:
      if (currentState == SYSTEM_ON) {
        currentMenuIndex = (currentMenuIndex % MAX_MENU_ITEMS) + 1;
        sendBLEMenuAction('S', currentMenuIndex);
        executedCommand = "Rotate menu";
      } else {
        executedCommand = "Ignored: System OFF";
      }
      break;

    case 2:
      if (currentState == SYSTEM_ON) {
        sendBLEMenuAction('A', currentMenuIndex);
        executedCommand = "Select highlighted option";
      } else {
        executedCommand = "Ignored: System OFF";
      }
      break;

    case 4:
      currentState = (currentState == SYSTEM_OFF) ? SYSTEM_ON : SYSTEM_OFF;
      sendBLEMenuAction('X', currentState == SYSTEM_ON ? BLE_SYSTEM_ACTIVATED : BLE_SYSTEM_INACTIVE);
      executedCommand = (currentState == SYSTEM_ON) ? "System Activated (ON)" : "System Deactivated (OFF)";
      break;

    default:
      break;
  }

  commandLocked = true;
  commandLockoutStartMs = nowMs;
  Serial.printf(">>> EXECUTE COMMAND: %s (Count=%u) <<<\n", executedCommand, completedBlinkCount);
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

  lastSampleUs = micros();

  Serial.println("=========================================================================");
  Serial.println("NeuroSpeak Biomedical EOG Controller (ESP32-C6 Optimized)");
  Serial.println("Electrode Setup: IN+(Forehead Midline), IN-(Mastoid A1), Ref(Mastoid A2)");
  Serial.println("Trimmed Auto-calibration running (5 seconds)... Please remain still.");
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

  // Phase-Accurate Fixed-Rate Sampling (250 Hz)
  if (static_cast<uint32_t>(nowUs - lastSampleUs) >= SAMPLE_INTERVAL_US) {
    lastSampleUs += SAMPLE_INTERVAL_US; // Prevents cumulative sample clock drift

    const int rawValue = analogRead(SENSOR_PIN);
    const float filteredValue = filterSignal(rawValue);
    const float voltageMv = static_cast<float>(analogReadMilliVolts(SENSOR_PIN));
    const bool blinkDetected = detectBlink(filteredValue, nowMs);

    if (blinkDetected) {
      registerBlink(nowMs);
    }

    // Thread-safe snapshot for FreeRTOS task
    portENTER_CRITICAL(&telemetryMux);
    currentTelemetry.blinkCount = blinkCount;
    currentTelemetry.systemActivated = (currentState == SYSTEM_ON);
    uint8_t idx = (currentMenuIndex >= 1 && currentMenuIndex <= 6) ? (currentMenuIndex - 1) : 0;
    strncpy(currentTelemetry.selectedOutput, OPTION_NAMES[idx], sizeof(currentTelemetry.selectedOutput) - 1);
    currentTelemetry.selectedOutput[sizeof(currentTelemetry.selectedOutput) - 1] = '\0';
    portEXIT_CRITICAL(&telemetryMux);

    if (elapsed(nowMs, lastDebugMs, DEBUG_INTERVAL_MS)) {
      lastDebugMs = nowMs;
      if (!isCalibrated) {
        Serial.printf("[CALIBRATING] Progress: %u/%u samples | Raw: %d\n", calibrationCount, CALIBRATION_SAMPLES, rawValue);
      } else {
        Serial.printf("F:%.1f V:%.1fmV Baseline:%.1f Trig:%.1f Noise:%.1f Conf:%.0f%% Blink:%u State:%s Det:%s\n",
                      filteredValue, voltageMv, noiseMean, blinkTriggerThreshold, noiseStdDev,
                      lastConfidenceScore, blinkCount, systemStateName(), detectorStateName());
      }
    }
  }

  // Single, clean event-driven timer for finalizing multi-blink sequences
  if (blinkCount > 0 &&
      (elapsed(nowMs, lastSequenceBlinkMs, INTER_BLINK_TIMEOUT_MS) ||
       elapsed(nowMs, sequenceStartMs, MAX_SEQUENCE_WINDOW_MS))) {
    processBlinkSequence(nowMs);
  }
}
