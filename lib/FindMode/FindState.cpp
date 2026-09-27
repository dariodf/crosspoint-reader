#include "FindState.h"

#include <CredentialIntegrity.h>

#include <cstddef>
#include <string_view>

namespace find_mode {

namespace {

uint32_t stateCrc(const FindState& state) {
  const std::string_view bytes(reinterpret_cast<const char*>(&state), offsetof(FindState, crc));
  return credential_integrity::crc32(bytes);
}

}  // namespace

void updateChecksum(FindState& state) {
  state.magic = STATE_MAGIC;
  state.version = STATE_VERSION;
  state.crc = stateCrc(state);
}

bool isStateValid(const FindState& state) {
  return state.magic == STATE_MAGIC && state.version == STATE_VERSION && state.crc == stateCrc(state);
}

TimerWakeAction decideTimerWake(const FindState& state, const uint16_t batteryPercent) {
  if (!isStateValid(state) || state.switchedOffByCrashes) return TimerWakeAction::NormalBoot;
  if (batteryPercent < state.minBatteryPercent) return TimerWakeAction::SleepUntilButton;
  return TimerWakeAction::Scan;
}

void enterFastPath(FindState& state) {
  state.inFastPath = 1;
  state.wakes++;
  updateChecksum(state);
}

void leaveFastPath(FindState& state, const uint32_t awakeMs, const bool detected) {
  state.inFastPath = 0;
  state.fastPathCrashes = 0;
  state.awakeMs += awakeMs;
  if (detected) state.detections++;
  updateChecksum(state);
}

void countFastPathCrash(FindState& state, const bool resetWasCrash) {
  if (!isStateValid(state) || !state.inFastPath) return;

  state.inFastPath = 0;
  if (resetWasCrash) {
    state.fastPathCrashes++;
    if (state.fastPathCrashes >= FAST_PATH_CRASHES_TO_SWITCH_OFF) state.switchedOffByCrashes = 1;
  }
  updateChecksum(state);
}

}  // namespace find_mode
