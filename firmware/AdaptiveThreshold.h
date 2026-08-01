#pragma once

#include <Arduino.h>
#include "Calibration.h"

class AdaptiveThreshold {
 public:
  void reset(const CalibrationResult &initial);
  void updateQuiet(float sample, bool quietEligible);
  float baseline() const { return baseline_; }
  float noise() const { return noise_; }
  float positiveTrigger() const { return baseline_ + 4.0f * noise_; }
  float positiveRelease() const { return baseline_ + 1.6f * noise_; }
  float negativeTrigger() const { return baseline_ - 4.0f * noise_; }
  float negativeRelease() const { return baseline_ - 1.6f * noise_; }

 private:
  float baseline_ = 0.0f;
  float noise_ = 1.0f;
};
