#pragma once

// Find mode: the Bluetooth side, on NimBLE.
//
// listenForCode() runs a passive scan: the radio only listens and sends
// nothing, so a sleeping reader never reveals itself until the owner's phone
// calls it. broadcastFound() is found mode: it broadcasts the name "CP-FIND"
// every 100 ms so a phone scanner can follow the signal strength.
//
// Both return early when `powerButtonPressed` reports a press, so the owner can
// always take the reader back. stopRadio() shuts NimBLE down and frees its
// memory; call it after either one.
//
// The NimBLE code builds only with CROSSPOINT_FIND_MODE; other builds carry no
// Bluetooth code.

#include <cstdint>

#include "FindCode.h"

namespace find_mode {

// Polled every few milliseconds while the radio runs.
using PowerButtonCheck = bool (*)();

enum class RadioResult {
  HeardCode,
  NothingHeard,  // the listening time ran out
  TimeUp,        // the broadcast time ran out
  ButtonPressed,
  RadioFailed,  // NimBLE could not start
};

static constexpr char FOUND_NAME[] = "CP-FIND";

RadioResult listenForCode(const Code& code, uint32_t listenMs, PowerButtonCheck powerButtonPressed);
// millis() when the last listenForCode() scan started, 0 when it never did.
// With the listen's end, gives how long the code took to be heard.
uint32_t listenStartedAtMs();
RadioResult broadcastFound(uint32_t broadcastMs, PowerButtonCheck powerButtonPressed);
void stopRadio();

}  // namespace find_mode
