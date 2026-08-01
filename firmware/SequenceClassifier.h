#pragma once
#include <Arduino.h>

enum class BlinkCommand : uint8_t { None, Single, Double, Quad };
class SequenceClassifier {
 public:
  BlinkCommand add(uint32_t nowMs);
  BlinkCommand poll(uint32_t nowMs);
  uint8_t count() const { return count_; }
  void reset();

 private:
  bool validQuad() const;
  uint32_t times_[4] = {0, 0, 0, 0};
  uint8_t count_ = 0;
};
