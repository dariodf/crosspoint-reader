#pragma once

// Find mode: what survives deep sleep, and what a timer wake does with it.
//
// Every find-mode wake is a full chip restart (deep sleep clears RAM), and the
// wake has to stay short: mounting the SD card to read settings.json would cost
// time and battery on every wake. So just before sleeping, the firmware copies
// the settings it needs into a FindState kept in RTC memory, the small slice of
// RAM that survives deep sleep (not a battery-empty power loss). On a timer
// wake the "fast path" reads only this struct.
//
// The flow on each timer wake:
//   1. isStateValid(): RTC memory is zero after power loss and could hold
//      stale bytes after a firmware change, so a CRC guards it.
//   2. decideTimerWake(): scan, go straight back to sleep (low battery), or
//      boot normally (state unusable, or the mode switched itself off).
//   3. enterFastPath() ... scan ... decideAfterScan() ... leaveFastPath().
//
// Found mode ends with the power button, which calls muteUntilCodeGone(): the
// phone may still be advertising, so later wakes ignore the code until one
// wake does not hear it, and then detection re-arms.
//
// Safety net: enterFastPath() raises a flag and leaveFastPath() clears it. If
// the chip resets while the flag is up and the reset was a crash,
// countFastPathCrash() counts it. Three in a row switch the mode off, so a bug
// in the scan cannot keep a reader crashing on every wake.
//
// Pure logic, no hardware: covered by the host tests in test/find_code/.

#include <cstdint>

#include "FindCode.h"

namespace find_mode {

static constexpr uint32_t STATE_MAGIC = 0xF1ADC0DE;
static constexpr uint8_t STATE_VERSION = 1;
static constexpr uint8_t FAST_PATH_CRASHES_TO_SWITCH_OFF = 3;

// Field order leaves no padding bytes, so the CRC covers every byte before
// `crc` and nothing uninitialised.
struct FindState {
  uint32_t magic;
  uint32_t wakes;       // timer wakes that entered the fast path
  uint32_t awakeMs;     // total time spent in the fast path (battery estimate)
  uint32_t detections;  // fast paths that heard the code
  Code code;
  uint8_t version;
  uint8_t intervalMinutes;
  uint8_t minBatteryPercent;
  uint8_t fastPathCrashes;  // consecutive crashes inside the fast path
  uint8_t inFastPath;       // raised on entry, cleared on a clean exit
  uint8_t switchedOffByCrashes;
  uint8_t mutedUntilCodeGone;  // set by the power button in found mode
  uint8_t reserved;
  uint32_t crc;
};
static_assert(sizeof(FindState) == 44, "FindState must have no padding");

// Stamps magic, version and checksum. Call after every change to the struct.
void updateChecksum(FindState& state);

// False for zeroed or corrupted RTC memory, or an older layout.
bool isStateValid(const FindState& state);

enum class TimerWakeAction {
  NormalBoot,        // state unusable or the mode switched itself off
  SleepUntilButton,  // battery below the minimum: stop waking on the timer
  Scan,
};

TimerWakeAction decideTimerWake(const FindState& state, uint16_t batteryPercent);

void enterFastPath(FindState& state);

enum class AfterScanAction {
  BroadcastFound,  // start found mode: broadcast CP-FIND
  Sleep,
};

// Decides what a finished scan leads to, and re-arms a muted mode once a scan
// no longer hears the code.
AfterScanAction decideAfterScan(FindState& state, bool heardCode);

void muteUntilCodeGone(FindState& state);
void leaveFastPath(FindState& state, uint32_t awakeMs, bool heardCode);

// Call early on every boot with whether the reset was a crash (panic or
// watchdog). Counts a crash only when the previous boot died inside the
// fast path.
void countFastPathCrash(FindState& state, bool resetWasCrash);

}  // namespace find_mode
