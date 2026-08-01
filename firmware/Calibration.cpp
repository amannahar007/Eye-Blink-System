#include "Calibration.h"
#include <math.h>

void Calibration::reset() { count_ = 0; }
bool Calibration::add(float sample) { if (count_ >= kSamples) return true; samples_[count_++] = sample; return complete(); }

float Calibration::select(float *data, int left, int right, int rank) {
  while (left < right) {
    const float pivot = data[right]; int store = left;
    for (int i = left; i < right; ++i) if (data[i] < pivot) { const float t = data[store]; data[store++] = data[i]; data[i] = t; }
    const float t = data[store]; data[store] = data[right]; data[right] = t;
    if (store == rank) break;
    if (rank < store) right = store - 1; else left = store + 1;
  }
  return data[rank];
}

CalibrationResult Calibration::calculate() {
  const int middle = kSamples / 2;
  const float median = select(samples_, 0, kSamples - 1, middle);
  double sumSquares = 0.0;
  for (uint16_t i = 0; i < kSamples; ++i) { const float d = samples_[i] - median; sumSquares += d * d; samples_[i] = fabsf(d); }
  const float mad = select(samples_, 0, kSamples - 1, middle);
  CalibrationResult result = {median, fmaxf(1.0f, 1.4826f * mad), fmaxf(1.0f, sqrtf(sumSquares / kSamples))};
  return result;
}
