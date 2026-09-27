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

void sealState(FindState& state) {
  state.magic = STATE_MAGIC;
  state.version = STATE_VERSION;
  state.crc = stateCrc(state);
}

bool isStateValid(const FindState& state) {
  return state.magic == STATE_MAGIC && state.version == STATE_VERSION && state.crc == stateCrc(state);
}

TimerWakeAction decideTimerWake(const FindState& state, const uint16_t batteryPercent) {
  if (!isStateValid(state) || state.disabledByFailures) return TimerWakeAction::NormalBoot;
  if (batteryPercent < state.minBatteryPercent) return TimerWakeAction::SleepUntilButton;
  return TimerWakeAction::Scan;
}

void enterFastPath(FindState& state) {
  state.inFastPath = 1;
  state.wakes++;
  sealState(state);
}

void leaveFastPath(FindState& state, const uint32_t awakeMs, const bool detected) {
  state.inFastPath = 0;
  state.fastPathFailures = 0;
  state.awakeMs += awakeMs;
  if (detected) state.detections++;
  sealState(state);
}

void noteBoot(FindState& state, const bool crashReset) {
  if (!isStateValid(state) || !state.inFastPath) return;

  state.inFastPath = 0;
  if (crashReset) {
    state.fastPathFailures++;
    if (state.fastPathFailures >= MAX_FAST_PATH_FAILURES) state.disabledByFailures = 1;
  }
  sealState(state);
}

}  // namespace find_mode
