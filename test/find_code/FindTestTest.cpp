#include <gtest/gtest.h>

#include "lib/FindMode/FindTest.h"

namespace {

find_mode::Code aCode() {
  find_mode::Code code{};
  code[0] = 0xC0;
  code[15] = 0x01;
  return code;
}

find_mode::TestRequest aRequest() {
  find_mode::TestRequest request{};
  find_mode::requestTest(request, aCode(), 1, 0);
  return request;
}

}  // namespace

TEST(FindTest, ZeroedMemoryHoldsNoTest) {
  find_mode::TestRequest request{};

  const bool taken = find_mode::takeTestRequest(request);

  EXPECT_FALSE(taken);
}

TEST(FindTest, ARequestIsTakenOnce) {
  find_mode::TestRequest request = aRequest();
  find_mode::takeTestRequest(request);

  const bool takenAgain = find_mode::takeTestRequest(request);

  EXPECT_FALSE(takenAgain);
}

TEST(FindTest, ATakenRequestKeepsTheCode) {
  find_mode::TestRequest request = aRequest();

  find_mode::takeTestRequest(request);

  EXPECT_EQ(request.code, aCode());
}

TEST(FindTest, ATestLeftRunningByAResetFails) {
  find_mode::TestRequest request = aRequest();
  find_mode::takeTestRequest(request);

  find_mode::takeTestRequest(request);

  EXPECT_EQ(find_mode::lastTestPhase(request), find_mode::TestPhase::Failed);
}

TEST(FindTest, HearingThePhoneKeepsItsSignal) {
  find_mode::TestRequest request = aRequest();
  find_mode::takeTestRequest(request);

  find_mode::finishTest(request, find_mode::TestPhase::Heard, -58);

  EXPECT_EQ(request.rssiMagnitude, 58);
}

TEST(FindTest, AFinishedTestReportsItsPhase) {
  find_mode::TestRequest request = aRequest();
  find_mode::takeTestRequest(request);

  find_mode::finishTest(request, find_mode::TestPhase::NothingHeard, 0);

  EXPECT_EQ(find_mode::lastTestPhase(request), find_mode::TestPhase::NothingHeard);
}

TEST(FindTest, ACorruptedRequestReportsNoTest) {
  find_mode::TestRequest request = aRequest();
  request.phase = static_cast<uint8_t>(find_mode::TestPhase::Heard);

  const find_mode::TestPhase phase = find_mode::lastTestPhase(request);

  EXPECT_EQ(phase, find_mode::TestPhase::None);
}

TEST(FindTestOutcome, HeardCodeIsHeard) {
  const find_mode::TestPhase phase = find_mode::testOutcome(find_mode::RadioResult::HeardCode);

  EXPECT_EQ(phase, find_mode::TestPhase::Heard);
}

TEST(FindTestOutcome, APressIsStopped) {
  const find_mode::TestPhase phase = find_mode::testOutcome(find_mode::RadioResult::ButtonPressed);

  EXPECT_EQ(phase, find_mode::TestPhase::Stopped);
}

TEST(FindTestOutcome, ARanOutListenIsNothingHeard) {
  const find_mode::TestPhase phase = find_mode::testOutcome(find_mode::RadioResult::NothingHeard);

  EXPECT_EQ(phase, find_mode::TestPhase::NothingHeard);
}

TEST(FindTestOutcome, ARadioFailureFails) {
  const find_mode::TestPhase phase = find_mode::testOutcome(find_mode::RadioResult::RadioFailed);

  EXPECT_EQ(phase, find_mode::TestPhase::Failed);
}
