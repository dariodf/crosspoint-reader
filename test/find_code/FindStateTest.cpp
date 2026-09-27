#include <gtest/gtest.h>

#include <cstdint>

#include "lib/FindMode/FindState.h"

namespace {

using find_mode::FindState;
using find_mode::TimerWakeAction;

// A reader set to wake every 2 minutes and scan while the battery is at 15 % or more.
FindState aSealedState() {
  FindState state{};
  state.intervalMinutes = 2;
  state.minBatteryPercent = 15;
  find_mode::sealState(state);
  return state;
}

FindState afterCrashesInTheFastPath(int crashes) {
  FindState state = aSealedState();
  for (int i = 0; i < crashes; i++) {
    find_mode::enterFastPath(state);
    find_mode::noteBoot(state, /*crashReset=*/true);
  }
  return state;
}

TEST(FindStateSeal, ASealedStateIsValid) {
  const FindState state = aSealedState();

  const bool valid = find_mode::isStateValid(state);

  EXPECT_TRUE(valid);
}

TEST(FindStateSeal, AZeroedStateIsInvalid) {
  // RTC memory after a battery-empty cold boot.
  const FindState state{};

  const bool valid = find_mode::isStateValid(state);

  EXPECT_FALSE(valid);
}

TEST(FindStateSeal, AChangedFieldBreaksTheSeal) {
  FindState state = aSealedState();
  state.minBatteryPercent = 5;

  const bool valid = find_mode::isStateValid(state);

  EXPECT_FALSE(valid);
}

TEST(FindStateSeal, AChangedCodeByteBreaksTheSeal) {
  FindState state = aSealedState();
  state.code[7] ^= 0x01;

  const bool valid = find_mode::isStateValid(state);

  EXPECT_FALSE(valid);
}

TEST(FindStateSeal, AnOlderVersionIsInvalid) {
  FindState state = aSealedState();
  state.version = find_mode::STATE_VERSION - 1;

  const bool valid = find_mode::isStateValid(state);

  EXPECT_FALSE(valid);
}

TEST(FindStateTimerWake, AValidStateScans) {
  const FindState state = aSealedState();

  const TimerWakeAction action = find_mode::decideTimerWake(state, 80);

  EXPECT_EQ(action, TimerWakeAction::Scan);
}

TEST(FindStateTimerWake, AnInvalidStateBootsNormally) {
  const FindState state{};

  const TimerWakeAction action = find_mode::decideTimerWake(state, 80);

  EXPECT_EQ(action, TimerWakeAction::NormalBoot);
}

TEST(FindStateTimerWake, BelowTheMinimumBatterySleepsUntilTheButton) {
  const FindState state = aSealedState();

  const TimerWakeAction action = find_mode::decideTimerWake(state, 14);

  EXPECT_EQ(action, TimerWakeAction::SleepUntilButton);
}

TEST(FindStateTimerWake, AtTheMinimumBatteryScans) {
  const FindState state = aSealedState();

  const TimerWakeAction action = find_mode::decideTimerWake(state, 15);

  EXPECT_EQ(action, TimerWakeAction::Scan);
}

TEST(FindStateFailures, TwoCrashesKeepTheModeOn) {
  const FindState state = afterCrashesInTheFastPath(2);

  const TimerWakeAction action = find_mode::decideTimerWake(state, 80);

  EXPECT_EQ(action, TimerWakeAction::Scan);
}

TEST(FindStateFailures, ThreeCrashesInARowSwitchTheModeOff) {
  const FindState state = afterCrashesInTheFastPath(3);

  const TimerWakeAction action = find_mode::decideTimerWake(state, 80);

  EXPECT_EQ(action, TimerWakeAction::NormalBoot);
}

TEST(FindStateFailures, ACleanFastPathResetsTheCount) {
  FindState state = afterCrashesInTheFastPath(2);
  find_mode::enterFastPath(state);
  find_mode::leaveFastPath(state, 900, false);
  find_mode::enterFastPath(state);
  find_mode::noteBoot(state, /*crashReset=*/true);

  const uint8_t failures = state.fastPathFailures;

  EXPECT_EQ(failures, 1);
}

TEST(FindStateFailures, ACrashOutsideTheFastPathIsNotCounted) {
  FindState state = aSealedState();
  find_mode::noteBoot(state, /*crashReset=*/true);

  const uint8_t failures = state.fastPathFailures;

  EXPECT_EQ(failures, 0);
}

TEST(FindStateFailures, AResetInsideTheFastPathWithoutACrashIsNotCounted) {
  // For example a button press that interrupts the scan.
  FindState state = aSealedState();
  find_mode::enterFastPath(state);
  find_mode::noteBoot(state, /*crashReset=*/false);

  const bool counted = state.fastPathFailures != 0 || state.inFastPath != 0;

  EXPECT_FALSE(counted);
}

TEST(FindStateCounters, TwoFastPathsAddUp) {
  FindState state = aSealedState();
  find_mode::enterFastPath(state);
  find_mode::leaveFastPath(state, 900, false);
  find_mode::enterFastPath(state);
  find_mode::leaveFastPath(state, 1100, true);

  const bool added = state.wakes == 2 && state.awakeMs == 2000 && state.detections == 1;

  EXPECT_TRUE(added && find_mode::isStateValid(state));
}

}  // namespace
