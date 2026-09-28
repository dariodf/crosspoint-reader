#pragma once

// Find mode: Test mode, run from the BLE Find Mode page.
//
// Bluetooth runs only early in boot, before its memory goes to the heap, so the
// page cannot listen by itself. It stores a TestRequest in RTC memory and
// restarts; the boot takes the request, listens for the owner's phone and, when
// it hears it, echoes CP-FIND like a real find. The result stays in the request
// for the page to show.
//
// A test that dies (crash or guard abort) leaves the request Running; the next
// boot turns that into Failed, so a broken test never runs again by itself.
//
// Pure logic, no hardware: covered by the host tests in test/find_code/.

#include <cstdint>

#include "FindCode.h"
#include "FindRadio.h"

namespace find_mode {

static constexpr uint32_t TEST_MAGIC = 0xF1AD7E57;

enum class TestPhase : uint8_t {
  None,          // no test since power-up
  Requested,     // the page asked; the next boot runs it
  Running,       // the boot is running it
  Heard,         // the reader heard the phone
  NothingHeard,  // the listen ran out
  Failed,        // the radio failed, or the test died
  Stopped,       // the owner pressed power before the phone was heard
};

// Field order leaves no padding bytes, so the CRC covers every byte before `crc`.
struct TestRequest {
  uint32_t magic;
  Code code;
  uint8_t language;
  uint8_t orientation;
  uint8_t phase;
  uint8_t rssiMagnitude;  // the phone's signal when heard, as -dBm (58 = -58 dBm)
  uint32_t crc;
};
static_assert(sizeof(TestRequest) == 28, "TestRequest must have no padding");

void requestTest(TestRequest& request, const Code& code, uint8_t language, uint8_t orientation);

// On boot: true when a test was requested, which is now Running. A test left
// Running by a reset becomes Failed.
bool takeTestRequest(TestRequest& request);

// What the listen's result means for the owner.
TestPhase testOutcome(RadioResult listenResult);

void finishTest(TestRequest& request, TestPhase phase, int8_t rssi);

// None for zeroed or corrupted RTC memory.
TestPhase lastTestPhase(const TestRequest& request);

}  // namespace find_mode
