#pragma once

#include <Arduino.h>
#include "AdaptiveThreshold.h"
#include "Calibration.h"
#include "Filters.h"
#include "StateMachine.h"

enum class BlinkRejectReason : uint8_t { None, TooShort, TooLong, Saturated, SlowMovement, BadRecovery, BadSymmetry, LowConfidence, NegativeDeflection };

class BlinkDetector {
 public:
  void begin();
  bool process(uint16_t raw, uint32_t nowMs);
  bool calibrated() const { return calibrated_; }
  float filtered() const { return filtered_; }
  float baseline() const { return threshold_.baseline(); }
  float noise() const { return threshold_.noise(); }
  float trigger() const { return threshold_.positiveTrigger(); }
  float negativeTrigger() const { return threshold_.negativeTrigger(); }
  float peak() const { return lastPeak_; }
  float negativePeak() const { return lastNegativePeak_; }
  float prominence() const { return lastPeak_ - threshold_.baseline(); }
  uint16_t widthMs() const { return lastWidthMs_; }
  float area() const { return lastArea_; }
  float energy() const { return lastEnergy_; }
  float confidence() const { return lastConfidence_; }
  DetectorState state() const { return state_; }
  BlinkRejectReason rejectReason() const { return lastReject_; }
  uint16_t calibrationCount() const { return calibration_.count(); }

 private:
  void startPulse(uint32_t nowMs);
  bool finishPulse(uint32_t nowMs);
  void reject(BlinkRejectReason reason, uint32_t nowMs);
  float score(uint32_t durationMs, uint32_t riseMs, uint32_t fallMs) const;

  AdaptiveDcRemoval dc_;
  HighPassFilter hpf_;
  LowPassFilter lpf_;
  MedianFilter3 median_;
  MovingAverage5 average_;
  Calibration calibration_;
  AdaptiveThreshold threshold_;
  DetectorState state_ = DetectorState::Calibrating;
  bool calibrated_ = false;
  uint32_t startMs_ = 0, peakMs_ = 0, refractoryStartMs_ = 0;
  float filtered_ = 0.0f, pulsePeak_ = 0.0f, pulseNegativePeak_ = 0.0f;
  float area_ = 0.0f, energy_ = 0.0f, riseSlope_ = 0.0f, fallSlope_ = 0.0f;
  float lastPeak_ = 0.0f, lastNegativePeak_ = 0.0f, lastArea_ = 0.0f, lastEnergy_ = 0.0f, lastConfidence_ = 0.0f;
  uint16_t lastWidthMs_ = 0;
  BlinkRejectReason lastReject_ = BlinkRejectReason::None;
  bool saturated_ = false;
};

const char *blinkRejectReasonName(BlinkRejectReason reason);
