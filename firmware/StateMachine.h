#pragma once
#include <Arduino.h>

enum class DetectorState : uint8_t { Calibrating, Ready, Rising, Falling, Refractory, Rejecting };
const char *detectorStateName(DetectorState state);
