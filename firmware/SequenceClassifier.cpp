#include "SequenceClassifier.h"
namespace { constexpr uint32_t kFinalizeMs = 500; constexpr uint32_t kMinGapMs = 180; constexpr uint32_t kMaxGapMs = 650; constexpr uint32_t kMaxQuadWindowMs = 2200; }
void SequenceClassifier::reset() { count_ = 0; }
bool SequenceClassifier::validQuad() const {
  if (count_ != 4 || static_cast<uint32_t>(times_[3] - times_[0]) > kMaxQuadWindowMs) return false;
  for (uint8_t i = 1; i < 4; ++i) { const uint32_t gap = times_[i] - times_[i - 1]; if (gap < kMinGapMs || gap > kMaxGapMs) return false; }
  return true;
}
BlinkCommand SequenceClassifier::add(uint32_t nowMs) {
  if (count_ && nowMs - times_[count_ - 1] > kFinalizeMs) reset();
  if (count_ < 4) times_[count_++] = nowMs;
  if (count_ == 4) { const bool valid = validQuad(); reset(); return valid ? BlinkCommand::Quad : BlinkCommand::None; }
  return BlinkCommand::None;
}
BlinkCommand SequenceClassifier::poll(uint32_t nowMs) {
  if (!count_ || nowMs - times_[count_ - 1] < kFinalizeMs) return BlinkCommand::None;
  const uint8_t completed = count_; reset();
  return completed == 1 ? BlinkCommand::Single : (completed == 2 ? BlinkCommand::Double : BlinkCommand::None);
}
