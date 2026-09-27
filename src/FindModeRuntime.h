#pragma once

// Find mode wiring between the firmware's boot/sleep paths and lib/FindMode.
//
//   setup():          findModeOnBoot() on every boot, then, on a timer wake,
//                     findModeRunTimerWake() before the SD card or display.
//   enterDeepSleep(): findModePrepareSleep() copies the current settings into
//                     RTC memory and arms the wake timer.
//
// The state lives in RTC_NOINIT memory: it survives deep sleep and crash
// resets (so a crash inside the fast path can be counted), and holds garbage
// after power loss, which the checksum rejects.

#include <HalGPIO.h>
#include <HalPowerManager.h>

// Counts a crash if the previous boot died inside the fast path, and re-arms
// the wake timer for any sleep this boot takes.
void findModeOnBoot(HalPowerManager& powerManager);

// Timer wake: listens for the code and, when heard, broadcasts CP-FIND. Sleeps
// again without returning, unless the power button was pressed or the mode
// cannot run; then it returns and setup() continues a normal boot.
void findModeRunTimerWake(HalGPIO& gpio, HalPowerManager& powerManager);

// Before a normal sleep: mirror the settings into RTC memory and arm the timer.
void findModePrepareSleep(HalPowerManager& powerManager);
