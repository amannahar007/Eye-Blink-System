#pragma once

#include <Arduino.h>

class AdaptiveDcRemoval {
 public:
  void reset(float initial);
  float process(float sample);

 private:
  float baseline_ = 0.0f;
  bool initialized_ = false;
};

class HighPassFilter {
 public:
  void reset(float initial = 0.0f);
  float process(float sample);

 private:
  float x1_ = 0.0f;
  float y1_ = 0.0f;
};

class LowPassFilter {
 public:
  void reset();
  float process(float sample);

 private:
  float x1_ = 0.0f, x2_ = 0.0f;
  float y1_ = 0.0f, y2_ = 0.0f;
};

class MedianFilter3 {
 public:
  void reset(float initial = 0.0f);
  float process(float sample);

 private:
  float window_[3] = {0.0f, 0.0f, 0.0f};
  uint8_t index_ = 0;
};

class MovingAverage5 {
 public:
  void reset(float initial = 0.0f);
  float process(float sample);

 private:
  float window_[5] = {0.0f};
  float sum_ = 0.0f;
  uint8_t index_ = 0;
};
