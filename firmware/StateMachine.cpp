#include "StateMachine.h"
const char *detectorStateName(DetectorState state) {
  switch (state) {
    case DetectorState::Calibrating: return "CAL";
    case DetectorState::Ready: return "READY";
    case DetectorState::Rising: return "RISE";
    case DetectorState::Falling: return "FALL";
    case DetectorState::Refractory: return "REF";
    case DetectorState::Rejecting: return "REJECT";
  }
  return "?";
}
