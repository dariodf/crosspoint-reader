#pragma once

// Find mode test builds (CROSSPOINT_FIND_MODE_TEST_HOOKS): what
// scripts/findmode/run_suite.py needs to check find mode with nobody touching
// the reader.
//
//   Journal   every wake records its steps in RTC memory (lib/FindMode/FindJournal)
//   Heartbeat FIND_AWAKE every 2 s while the fast path runs, so a computer knows
//             a found-mode broadcast is under way even when it missed its start
//   Commands  serial "CMD:FIND_*" lines, read in loop() and, while the fast path
//             runs, every time it checks the power button:
//               FIND_PRESS            a virtual power-button press
//               FIND_JOURNAL          print the journal
//               FIND_JOURNAL_CLEAR    empty it
//               FIND_STATE            print the find-mode state
//               FIND_INJECT <kind> [value]   fault for the next timer wake:
//                                     crash | hang | radiofail | battery <percent>
//               FIND_RETRY            clear a crash switch-off
//             main.cpp adds FIND_SLEEP and FIND_PREVIEW <language> <orientation>.
//
// Without the flag every function here is an empty inline, so product builds
// carry none of it.

#include <FindJournal.h>

#include <cstdint>

enum class FindTestInjection : uint8_t {
  None = 0,
  Crash,      // abort inside the fast path: counts as a fast-path crash
  Hang,       // spin inside the fast path until the guard aborts it
  RadioFail,  // report a radio that failed to start
  Battery,    // report the injected battery percentage
};

#if CROSSPOINT_FIND_MODE_TEST_HOOKS

void findTestRecord(find_mode::JournalEvent event, uint8_t detail, uint32_t value);

// The fault set by FIND_INJECT, consumed by one timer wake. `battery` is set for
// FindTestInjection::Battery.
FindTestInjection findTestTakeInjection(uint16_t& battery);

// Reads pending serial commands; true when one of them was FIND_PRESS.
bool findTestVirtualPress();

// A "CMD:" line from loop(), with the prefix removed. True when handled.
bool findTestCommand(const char* command);

#else

inline void findTestRecord(find_mode::JournalEvent, uint8_t, uint32_t) {}
inline FindTestInjection findTestTakeInjection(uint16_t&) { return FindTestInjection::None; }
inline bool findTestVirtualPress() { return false; }
inline bool findTestCommand(const char*) { return false; }

#endif
