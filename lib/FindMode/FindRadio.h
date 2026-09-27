#pragma once

// Find mode: the Bluetooth side, on NimBLE.
//
// listenForCode() runs a passive scan: the radio only listens and sends
// nothing, so a sleeping reader never reveals itself until the owner's phone
// calls it. announceFound() is found mode: it broadcasts the name "CP-FIND"
// every 100 ms so a phone scanner can follow the signal strength.
//
// Both return early when `buttonPressed` reports the power button, so the owner
// can always take the reader back. radioOff() shuts NimBLE down and frees its
// memory; call it before sleeping.
//
// The NimBLE code builds only with CROSSPOINT_FIND_MODE; other builds carry no
// Bluetooth code.

#include <cstdint>

#include "FindCode.h"

namespace find_mode {

// Polled every few milliseconds while the radio runs.
using ButtonCheck = bool (*)();

enum class RadioResult {
  HeardCode,
  Quiet,  // scan window or broadcast time ran out
  ButtonPressed,
  RadioFailed,  // NimBLE could not start
};

static constexpr char FOUND_NAME[] = "CP-FIND";

RadioResult listenForCode(const Code& code, uint32_t windowMs, ButtonCheck buttonPressed);
RadioResult announceFound(uint32_t durationMs, ButtonCheck buttonPressed);
void radioOff();

}  // namespace find_mode
