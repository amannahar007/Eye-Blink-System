#include "AdaptiveThreshold.h"
#include <math.h>

void AdaptiveThreshold::reset(const CalibrationResult &initial) {
  baseline_ = initial.baseline;
  noise_ = fmaxf(1.0f, fmaxf(initial.madSigma, 0.5f * initial.rmsNoise));
}
void AdaptiveThreshold::updateQuiet(float sample, bool quietEligible) {
  if (!quietEligible) return;
  const float residual = sample - baseline_;
  if (fabsf(residual) > 3.0f * noise_) return;
  constexpr float kBeta = 0.001f;
  baseline_ += kBeta * residual;
  noise_ += kBeta * (fabsf(residual) - noise_);
  if (noise_ < 1.0f) noise_ = 1.0f;
}
