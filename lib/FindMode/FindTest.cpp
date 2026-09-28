#include "FindTest.h"

#include <CredentialIntegrity.h>

#include <cstddef>
#include <string_view>

namespace find_mode {

namespace {

uint32_t requestCrc(const TestRequest& request) {
  const std::string_view bytes(reinterpret_cast<const char*>(&request), offsetof(TestRequest, crc));
  return credential_integrity::crc32(bytes);
}

void seal(TestRequest& request) {
  request.magic = TEST_MAGIC;
  request.crc = requestCrc(request);
}

bool isValid(const TestRequest& request) { return request.magic == TEST_MAGIC && request.crc == requestCrc(request); }

}  // namespace

void requestTest(TestRequest& request, const Code& code, const uint8_t language, const uint8_t orientation) {
  request = {};
  request.code = code;
  request.language = language;
  request.orientation = orientation;
  request.phase = static_cast<uint8_t>(TestPhase::Requested);
  seal(request);
}

bool takeTestRequest(TestRequest& request) {
  if (!isValid(request)) return false;
  switch (static_cast<TestPhase>(request.phase)) {
    case TestPhase::Requested:
      request.phase = static_cast<uint8_t>(TestPhase::Running);
      seal(request);
      return true;
    case TestPhase::Running:
      request.phase = static_cast<uint8_t>(TestPhase::Failed);
      seal(request);
      return false;
    default:
      return false;
  }
}

TestPhase testOutcome(const RadioResult listenResult) {
  switch (listenResult) {
    case RadioResult::HeardCode:
      return TestPhase::Heard;
    case RadioResult::ButtonPressed:
      return TestPhase::Stopped;
    case RadioResult::NothingHeard:
    case RadioResult::TimeUp:
      return TestPhase::NothingHeard;
    default:
      return TestPhase::Failed;
  }
}

void finishTest(TestRequest& request, const TestPhase phase, const int8_t rssi) {
  request.phase = static_cast<uint8_t>(phase);
  request.rssiMagnitude = phase == TestPhase::Heard ? static_cast<uint8_t>(-rssi) : 0;
  seal(request);
}

TestPhase lastTestPhase(const TestRequest& request) {
  return isValid(request) ? static_cast<TestPhase>(request.phase) : TestPhase::None;
}

}  // namespace find_mode
