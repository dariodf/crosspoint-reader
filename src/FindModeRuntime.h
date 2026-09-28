#pragma once

// Find mode wiring between the firmware's boot/sleep paths and lib/FindMode.
//
//   setup():          findModeOnBoot() on every boot, then, on a timer wake,
//                     findModeRunTimerWake() before the SD card or display;
//                     findModeSettingsLoaded() once settings.json is read.
//   enterDeepSleep(): findModePrepareSleep() copies the current settings into
//                     RTC memory and arms the wake timer.
//   BLE Find Mode page: findModeStatus(),
//                     findModeEnsureCode(), findModeNewCode(),
//                     findModeRetryAfterSwitchOff(), and the phone test:
//                     findModeRequestTest(), then findModeLastTest().
//   setup(), before Bluetooth's memory goes to the heap: findModeRunTest().
//
// The state lives in RTC_NOINIT memory: it survives deep sleep and crash
// resets (so a crash inside the fast path can be counted), and holds garbage
// after power loss, which the checksum rejects.

#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>

#include <cstdint>

// Counts a crash if the previous boot died inside the fast path, and re-arms
// the wake timer for any sleep this boot takes. After a brownout inside the
// fast path it sleeps until the power button instead of returning.
void findModeOnBoot(HalGPIO& gpio, HalPowerManager& powerManager);

// Draws the found screen ("Find mode activated. Press power to close.") in the given
// UI language and CrossPointSettings orientation, and puts the display back to
// sleep. Supplied by main.cpp, which owns the display and fonts.
using FindModeFoundScreen = void (*)(uint8_t language, uint8_t orientation);

// Timer wake: listens for the code and, when heard, shows the found screen
// (once per search) and broadcasts CP-FIND. Sleeps again without returning,
// unless the owner pressed the power button (or the mode cannot run); then it
// returns and setup() continues as a power-button wake.
void findModeRunTimerWake(HalGPIO& gpio, HalPowerManager& powerManager, FindModeFoundScreen showFoundScreen);

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

// Creates a code when the stored one is missing or not valid UUID text (for
// example edited by hand on the web settings page).
void findModeEnsureCode();

// Replaces the code with 16 fresh random bytes, resets the counters and saves
// the settings. A phone set up with the old code no longer finds the reader.
void findModeNewCode();

// Clears a crash switch-off so the next sleeps listen again.
void findModeRetryAfterSwitchOff();

// Draws a two-part message (a bold heading over an instruction) the way the
// found screen does, and puts the display back to sleep. A heading with %d
// shows `value`. Supplied by main.cpp.
using FindModeMessageScreen = void (*)(uint8_t language, uint8_t orientation, StrId heading, StrId instruction,
                                       int value);

// Test mode: saves the settings, asks the next boot to run the test,
// and restarts into Settings.
void findModeRequestTest();

// Early in setup(): runs a requested test. The screen shows it listening for up
// to a minute; when it hears the phone, the signal and CP-FIND broadcasting
// until the phone stops or the owner presses power. True when the owner
// pressed power, whose release must not also act.
bool findModeRunTest(HalGPIO& gpio, FindModeMessageScreen showMessage);

enum class FindModeTestResult { None, Heard, NothingHeard, Failed, Stopped };
// The last test's result; `rssi` is the phone's signal when heard.
FindModeTestResult findModeLastTest(int& rssi);

#if CROSSPOINT_FIND_MODE_TEST_HOOKS
// Prints the state as one "FIND_STATE key=value ..." line (test builds).
void findModePrintState();
#endif
