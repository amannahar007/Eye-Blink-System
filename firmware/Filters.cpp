#include "Filters.h"

namespace {
constexpr float kDcBeta = 0.0004f;  // 0.2 s-1 tracking at 500 Hz.
constexpr float kHpfAlpha = 0.9937365f;  // exp(-2*pi*0.5/500).
// 10 Hz, 2nd-order Butterworth, fs = 500 Hz.
constexpr float kLpfB0 = 0.00362168f;
constexpr float kLpfB1 = 0.00724336f;
constexpr float kLpfB2 = 0.00362168f;
constexpr float kLpfA1 = -1.82269493f;
constexpr float kLpfA2 = 0.83718165f;
}

void AdaptiveDcRemoval::reset(float initial) { baseline_ = initial; initialized_ = true; }
float AdaptiveDcRemoval::process(float sample) {
  if (!initialized_) reset(sample);
  baseline_ += kDcBeta * (sample - baseline_);
  return sample - baseline_;
}

void HighPassFilter::reset(float initial) { x1_ = initial; y1_ = 0.0f; }
float HighPassFilter::process(float sample) {
  const float y = kHpfAlpha * (y1_ + sample - x1_);
  x1_ = sample; y1_ = y;
  return y;
}

void LowPassFilter::reset() { x1_ = x2_ = y1_ = y2_ = 0.0f; }
float LowPassFilter::process(float sample) {
  const float y = kLpfB0 * sample + kLpfB1 * x1_ + kLpfB2 * x2_ - kLpfA1 * y1_ - kLpfA2 * y2_;
  x2_ = x1_; x1_ = sample; y2_ = y1_; y1_ = y;
  return y;
}

void MedianFilter3::reset(float initial) { window_[0] = window_[1] = window_[2] = initial; index_ = 0; }
float MedianFilter3::process(float sample) {
  window_[index_] = sample; index_ = (index_ + 1) % 3;
  float a = window_[0], b = window_[1], c = window_[2];
  if (a > b) { const float t = a; a = b; b = t; }
  if (b > c) { const float t = b; b = c; c = t; }
  if (a > b) { const float t = a; a = b; b = t; }
  return b;
}

void MovingAverage5::reset(float initial) {
  sum_ = initial * 5.0f; index_ = 0;
  for (uint8_t i = 0; i < 5; ++i) window_[i] = initial;
}
float MovingAverage5::process(float sample) {
  sum_ -= window_[index_]; window_[index_] = sample; sum_ += sample;
  index_ = (index_ + 1) % 5;
  return sum_ * 0.2f;
}
