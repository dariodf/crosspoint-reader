#include <gtest/gtest.h>

#include <cstdint>

#include "lib/FindMode/FindState.h"

namespace {

using find_mode::AfterScanAction;
using find_mode::FindState;
using find_mode::TimerWakeAction;

// A reader set to wake every 2 minutes and scan while the battery is at 15 % or more.
FindState aValidState() {
  FindState state{};
  state.intervalMinutes = 2;
  state.minBatteryPercent = 15;
  find_mode::updateChecksum(state);
  return state;
}

FindState afterCrashesInTheFastPath(int crashes) {
  FindState state = aValidState();
  for (int i = 0; i < crashes; i++) {
    find_mode::enterFastPath(state);
    find_mode::countFastPathCrash(state, /*resetWasCrash=*/true);
  }
  return state;
}

TEST(FindStateChecksum, AFreshChecksumIsValid) {
  const FindState state = aValidState();

  const bool valid = find_mode::isStateValid(state);

  EXPECT_TRUE(valid);
}

TEST(FindStateChecksum, AZeroedStateIsInvalid) {
  // RTC memory after a battery-empty cold boot.
  const FindState state{};

  const bool valid = find_mode::isStateValid(state);

  EXPECT_FALSE(valid);
}

TEST(FindStateChecksum, AChangedFieldBreaksTheChecksum) {
  FindState state = aValidState();
  state.minBatteryPercent = 5;

  const bool valid = find_mode::isStateValid(state);

  EXPECT_FALSE(valid);
}

TEST(FindStateChecksum, AChangedCodeByteBreaksTheChecksum) {
  FindState state = aValidState();
  state.code[7] ^= 0x01;

  const bool valid = find_mode::isStateValid(state);

  EXPECT_FALSE(valid);
}

TEST(FindStateChecksum, AnOlderVersionIsInvalid) {
  FindState state = aValidState();
  state.version = find_mode::STATE_VERSION - 1;

  const bool valid = find_mode::isStateValid(state);

  EXPECT_FALSE(valid);
}

TEST(FindStateTimerWake, AValidStateScans) {
  const FindState state = aValidState();

  const TimerWakeAction action = find_mode::decideTimerWake(state, 80);

  EXPECT_EQ(action, TimerWakeAction::Scan);
}

TEST(FindStateTimerWake, AnInvalidStateBootsNormally) {
  const FindState state{};

  const TimerWakeAction action = find_mode::decideTimerWake(state, 80);

  EXPECT_EQ(action, TimerWakeAction::NormalBoot);
}

TEST(FindStateTimerWake, BelowTheMinimumBatterySleepsUntilTheButton) {
  const FindState state = aValidState();

  const TimerWakeAction action = find_mode::decideTimerWake(state, 14);

  EXPECT_EQ(action, TimerWakeAction::SleepUntilButton);
}

TEST(FindStateTimerWake, AtTheMinimumBatteryScans) {
  const FindState state = aValidState();

  const TimerWakeAction action = find_mode::decideTimerWake(state, 15);

  EXPECT_EQ(action, TimerWakeAction::Scan);
}

TEST(FindStateCrashes, TwoCrashesKeepTheModeOn) {
  const FindState state = afterCrashesInTheFastPath(2);

  const TimerWakeAction action = find_mode::decideTimerWake(state, 80);

  EXPECT_EQ(action, TimerWakeAction::Scan);
}

TEST(FindStateCrashes, ThreeCrashesInARowSwitchTheModeOff) {
  const FindState state = afterCrashesInTheFastPath(3);

  const TimerWakeAction action = find_mode::decideTimerWake(state, 80);

  EXPECT_EQ(action, TimerWakeAction::NormalBoot);
}

TEST(FindStateCrashes, ACleanFastPathResetsTheCount) {
  FindState state = afterCrashesInTheFastPath(2);
  find_mode::enterFastPath(state);
  find_mode::leaveFastPath(state, 900, false);
  find_mode::enterFastPath(state);
  find_mode::countFastPathCrash(state, /*resetWasCrash=*/true);

  const uint8_t crashes = state.fastPathCrashes;

  EXPECT_EQ(crashes, 1);
}

TEST(FindStateCrashes, ACrashOutsideTheFastPathIsNotCounted) {
  FindState state = aValidState();
  find_mode::countFastPathCrash(state, /*resetWasCrash=*/true);

  const uint8_t crashes = state.fastPathCrashes;

  EXPECT_EQ(crashes, 0);
}

TEST(FindStateCrashes, AResetInsideTheFastPathWithoutACrashIsNotCounted) {
  // For example a button press that interrupts the scan.
  FindState state = aValidState();
  find_mode::enterFastPath(state);
  find_mode::countFastPathCrash(state, /*resetWasCrash=*/false);

  const bool counted = state.fastPathCrashes != 0 || state.inFastPath != 0;

  EXPECT_FALSE(counted);
}

TEST(FindStateScan, HearingTheCodeBroadcastsFound) {
  FindState state = aValidState();

  const AfterScanAction action = find_mode::decideAfterScan(state, /*heardCode=*/true);

  EXPECT_EQ(action, AfterScanAction::BroadcastFound);
}

TEST(FindStateScan, SilenceSleeps) {
  FindState state = aValidState();

  const AfterScanAction action = find_mode::decideAfterScan(state, /*heardCode=*/false);

  EXPECT_EQ(action, AfterScanAction::Sleep);
}

TEST(FindStateScan, AfterThePowerButtonTheCodeIsIgnored) {
  FindState state = aValidState();
  find_mode::muteUntilCodeGone(state);

  const AfterScanAction action = find_mode::decideAfterScan(state, /*heardCode=*/true);

  EXPECT_EQ(action, AfterScanAction::Sleep);
}

TEST(FindStateScan, OneQuietScanReArmsDetection) {
  FindState state = aValidState();
  find_mode::muteUntilCodeGone(state);
  find_mode::decideAfterScan(state, /*heardCode=*/false);

  const AfterScanAction action = find_mode::decideAfterScan(state, /*heardCode=*/true);

  EXPECT_EQ(action, AfterScanAction::BroadcastFound);
}

TEST(FindStateScan, AMutedStateKeepsAFreshChecksum) {
  FindState state = aValidState();
  find_mode::muteUntilCodeGone(state);

  const bool valid = find_mode::isStateValid(state);

  EXPECT_TRUE(valid);
}

TEST(FindStateCounters, TwoFastPathsAddUp) {
  FindState state = aValidState();
  find_mode::enterFastPath(state);
  find_mode::leaveFastPath(state, 900, false);
  find_mode::enterFastPath(state);
  find_mode::leaveFastPath(state, 1100, true);

  const bool added = state.wakes == 2 && state.awakeMs == 2000 && state.detections == 1;

  EXPECT_TRUE(added && find_mode::isStateValid(state));
}

}  // namespace
