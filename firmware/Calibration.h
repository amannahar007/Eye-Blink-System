#pragma once

#include <Arduino.h>

struct CalibrationResult { float baseline; float madSigma; float rmsNoise; };

class Calibration {
 public:
  static constexpr uint16_t kSamples = 2000;  // Four seconds at 500 Hz.
  void reset();
  bool add(float sample);
  bool complete() const { return count_ >= kSamples; }
  CalibrationResult calculate();
  uint16_t count() const { return count_; }

 private:
  static float select(float *data, int left, int right, int rank);
  float samples_[kSamples] = {0.0f};
  uint16_t count_ = 0;
};
