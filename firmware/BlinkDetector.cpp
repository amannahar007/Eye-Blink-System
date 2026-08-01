#include "BlinkDetector.h"
#include <math.h>

namespace {
constexpr uint32_t kMinPulseMs = 55;
constexpr uint32_t kMaxPulseMs = 360;
constexpr uint32_t kRefractoryMs = 250;
constexpr float kSamplePeriodSeconds = 0.002f;
constexpr uint16_t kSaturationLow = 3;
constexpr uint16_t kSaturationHigh = 4092;
}

const char *blinkRejectReasonName(BlinkRejectReason reason) {
  switch (reason) {
    case BlinkRejectReason::None: return "NONE";
    case BlinkRejectReason::TooShort: return "SHORT";
    case BlinkRejectReason::TooLong: return "LONG";
    case BlinkRejectReason::Saturated: return "SAT";
    case BlinkRejectReason::SlowMovement: return "EYE_MOVE";
    case BlinkRejectReason::BadRecovery: return "RECOVERY";
    case BlinkRejectReason::BadSymmetry: return "SHAPE";
    case BlinkRejectReason::LowConfidence: return "CONF";
    case BlinkRejectReason::NegativeDeflection: return "NEGATIVE";
  }
  return "?";
}

void BlinkDetector::begin() {
  calibration_.reset(); calibrated_ = false; state_ = DetectorState::Calibrating;
  // DC tracker self-initializes from the first ADC sample, preventing a startup step.
  hpf_.reset(); lpf_.reset(); median_.reset(); average_.reset();
}

void BlinkDetector::startPulse(uint32_t nowMs) {
  startMs_ = peakMs_ = nowMs; pulsePeak_ = filtered_; pulseNegativePeak_ = filtered_;
  area_ = energy_ = riseSlope_ = fallSlope_ = 0.0f; saturated_ = false;
  lastReject_ = BlinkRejectReason::None; state_ = DetectorState::Rising;
}

void BlinkDetector::reject(BlinkRejectReason reason, uint32_t nowMs) {
  lastReject_ = reason; lastPeak_ = pulsePeak_; lastNegativePeak_ = pulseNegativePeak_;
  lastWidthMs_ = static_cast<uint16_t>(nowMs - startMs_); lastArea_ = area_; lastEnergy_ = energy_; lastConfidence_ = 0.0f;
  state_ = DetectorState::Refractory; refractoryStartMs_ = nowMs;
}

float BlinkDetector::score(uint32_t durationMs, uint32_t riseMs, uint32_t fallMs) const {
  const float prominence = pulsePeak_ - threshold_.baseline();
  const float snr = prominence / fmaxf(1.0f, threshold_.noise());
  const float amplitude = fminf(1.0f, fmaxf(0.0f, (snr - 4.0f) / 4.0f));
  const float duration = fmaxf(0.0f, 1.0f - fabsf(static_cast<float>(durationMs) - 150.0f) / 150.0f);
  const float ratio = fallMs ? static_cast<float>(riseMs) / fallMs : 0.0f;
  const float symmetry = fmaxf(0.0f, 1.0f - fabsf(ratio - 0.7f) / 1.2f);
  const float energy = fminf(1.0f, energy_ / fmaxf(1.0f, 12.0f * threshold_.noise() * threshold_.noise() * kSamplePeriodSeconds));
  return 100.0f * (0.35f * amplitude + 0.25f * duration + 0.20f * symmetry + 0.20f * energy);
}

bool BlinkDetector::finishPulse(uint32_t nowMs) {
  const uint32_t duration = nowMs - startMs_;
  const uint32_t rise = peakMs_ - startMs_;
  const uint32_t fall = nowMs - peakMs_;
  lastPeak_ = pulsePeak_; lastNegativePeak_ = pulseNegativePeak_; lastWidthMs_ = static_cast<uint16_t>(duration);
  lastArea_ = area_; lastEnergy_ = energy_; lastConfidence_ = score(duration, rise, fall);
  if (saturated_) { reject(BlinkRejectReason::Saturated, nowMs); return false; }
  if (duration < kMinPulseMs) { reject(BlinkRejectReason::TooShort, nowMs); return false; }
  if (duration > kMaxPulseMs) { reject(BlinkRejectReason::TooLong, nowMs); return false; }
  // Slow broad deflections are typical gaze/head movement; blinks have a distinct onset.
  if (rise == 0 || riseSlope_ < 0.08f * threshold_.noise()) { reject(BlinkRejectReason::SlowMovement, nowMs); return false; }
  const float ratio = fall ? static_cast<float>(rise) / fall : 0.0f;
  if (ratio < 0.12f || ratio > 3.5f) { reject(BlinkRejectReason::BadSymmetry, nowMs); return false; }
  if (fallSlope_ < 0.02f * threshold_.noise()) { reject(BlinkRejectReason::BadRecovery, nowMs); return false; }
  if (lastConfidence_ < 55.0f) { reject(BlinkRejectReason::LowConfidence, nowMs); return false; }
  state_ = DetectorState::Refractory; refractoryStartMs_ = nowMs; lastReject_ = BlinkRejectReason::None;
  return true;
}

bool BlinkDetector::process(uint16_t raw, uint32_t nowMs) {
  const float dcFree = dc_.process(static_cast<float>(raw));
  const float highPassed = hpf_.process(dcFree);
  const float lowPassed = lpf_.process(highPassed);
  filtered_ = average_.process(median_.process(lowPassed));
  if (!calibrated_) {
    if (calibration_.add(filtered_)) { threshold_.reset(calibration_.calculate()); calibrated_ = true; state_ = DetectorState::Ready; }
    return false;
  }

  if (state_ == DetectorState::Ready) {
    threshold_.updateQuiet(filtered_, true);
    if (filtered_ >= threshold_.positiveTrigger()) startPulse(nowMs);
    else if (filtered_ <= threshold_.negativeTrigger()) { pulsePeak_ = threshold_.baseline(); pulseNegativePeak_ = filtered_; reject(BlinkRejectReason::NegativeDeflection, nowMs); }
    return false;
  }
  if (state_ == DetectorState::Refractory) {
    if (nowMs - refractoryStartMs_ >= kRefractoryMs && filtered_ < threshold_.positiveRelease() && filtered_ > threshold_.negativeRelease()) state_ = DetectorState::Ready;
    return false;
  }
  if (state_ != DetectorState::Rising && state_ != DetectorState::Falling) return false;

  if (raw <= kSaturationLow || raw >= kSaturationHigh) saturated_ = true;
  const float deviation = filtered_ - threshold_.baseline();
  area_ += fmaxf(0.0f, deviation) * kSamplePeriodSeconds;
  energy_ += deviation * deviation * kSamplePeriodSeconds;
  if (filtered_ < pulseNegativePeak_) pulseNegativePeak_ = filtered_;
  if (filtered_ > pulsePeak_) {
    const uint32_t deltaMs = nowMs - peakMs_;
    if (deltaMs) riseSlope_ = fmaxf(riseSlope_, (filtered_ - pulsePeak_) / deltaMs);
    pulsePeak_ = filtered_; peakMs_ = nowMs; state_ = DetectorState::Rising;
  } else if (filtered_ < pulsePeak_ - 0.12f * (pulsePeak_ - threshold_.baseline())) {
    state_ = DetectorState::Falling;
    const uint32_t sincePeak = nowMs - peakMs_;
    if (sincePeak) fallSlope_ = fmaxf(fallSlope_, (pulsePeak_ - filtered_) / sincePeak);
  }
  if (nowMs - startMs_ > kMaxPulseMs) { reject(BlinkRejectReason::TooLong, nowMs); return false; }
  if (state_ == DetectorState::Falling && filtered_ <= threshold_.positiveRelease()) return finishPulse(nowMs);
  return false;
}
