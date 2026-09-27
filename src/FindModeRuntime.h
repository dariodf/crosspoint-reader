#pragma once

// Find mode wiring between the firmware's boot/sleep paths and lib/FindMode.
//
//   setup():          findModeOnBoot() on every boot, then, on a timer wake,
//                     findModeRunTimerWake() before the SD card or display;
//                     findModeSettingsLoaded() once settings.json is read.
//   enterDeepSleep(): findModePrepareSleep() copies the current settings into
//                     RTC memory and arms the wake timer.
//   Find mode code screen: findModeStatus(), findModeStats(),
//                     findModeEnsureCode(), findModeNewCode(),
//                     findModeRetryAfterSwitchOff().
//
// The state lives in RTC_NOINIT memory: it survives deep sleep and crash
// resets (so a crash inside the fast path can be counted), and holds garbage
// after power loss, which the checksum rejects.

#include <HalGPIO.h>
#include <HalPowerManager.h>

#include <cstdint>

// Counts a crash if the previous boot died inside the fast path, and re-arms
// the wake timer for any sleep this boot takes.
void findModeOnBoot(HalPowerManager& powerManager);

// Timer wake: listens for the code and, when heard, broadcasts CP-FIND. Sleeps
// again without returning, unless the owner pressed the power button (or the
// mode cannot run); then it returns and setup() continues as a power-button
// wake.
void findModeRunTimerWake(HalGPIO& gpio, HalPowerManager& powerManager);

// settings.json has been read. Until then findModePrepareSleep() keeps the
// state as it is: a boot that failed to mount the SD card must not read the
// default "off" and wipe the mode.
void findModeSettingsLoaded();

// Before a normal sleep: mirror the settings into RTC memory and arm the timer.
void findModePrepareSleep(HalPowerManager& powerManager);

enum class FindModeStatus {
  Off,          // the setting is off
  Listening,    // listens on every timer wake while asleep
  SwitchedOff,  // switched itself off after repeated crashes; needs a retry
};
FindModeStatus findModeStatus();

// Counters for the sleeps since the code last changed. Reset after power loss.
struct FindModeStats {
  uint32_t wakes;
  uint32_t detections;
  uint32_t awakeMs;
};
FindModeStats findModeStats();

// Creates a code when the stored one is missing or not valid UUID text (for
// example edited by hand on the web settings page).
void findModeEnsureCode();

// Replaces the code with 16 fresh random bytes, resets the counters and saves
// the settings. A phone set up with the old code no longer finds the reader.
void findModeNewCode();

// Clears a crash switch-off so the next sleeps listen again.
void findModeRetryAfterSwitchOff();
