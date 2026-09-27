#include "FindModeRuntime.h"

#if CROSSPOINT_FIND_MODE

#include <Arduino.h>
#include <FindRadio.h>
#include <FindState.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace {

static constexpr uint32_t SCAN_WINDOW_MS = 1000;
static constexpr uint32_t FOUND_MODE_MS = 5UL * 60UL * 1000UL;

RTC_NOINIT_ATTR find_mode::FindState rtcState;

HalGPIO* buttonGpio = nullptr;

bool powerButtonPressed() {
  buttonGpio->update();
  return buttonGpio->isPressed(HalGPIO::BTN_POWER);
}

bool resetWasCrash() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;
  }
}

// Settings the mode runs with. Test builds take them from build flags until the
// settings screen exists.
struct FindSettings {
  bool enabled;
  uint8_t intervalMinutes;
  uint8_t minBatteryPercent;
  find_mode::Code code;
};

FindSettings currentSettings() {
  FindSettings settings{};
#ifdef CROSSPOINT_FIND_MODE_TEST_CODE
  settings.enabled = find_mode::parseCode(CROSSPOINT_FIND_MODE_TEST_CODE, settings.code);
  settings.intervalMinutes = CROSSPOINT_FIND_MODE_TEST_INTERVAL_MINUTES;
  settings.minBatteryPercent = CROSSPOINT_FIND_MODE_TEST_MIN_BATTERY;
#endif
  return settings;
}

void armWakeTimer(HalPowerManager& powerManager) {
  const bool armed = find_mode::isStateValid(rtcState) && !rtcState.switchedOffByCrashes;
  powerManager.setWakeTimer(armed ? rtcState.intervalMinutes * 60U : 0);
}

}  // namespace

void findModeOnBoot(HalPowerManager& powerManager) {
  find_mode::countFastPathCrash(rtcState, resetWasCrash());
  armWakeTimer(powerManager);
}

void findModeRunTimerWake(HalGPIO& gpio, HalPowerManager& powerManager) {
  const uint32_t startedAt = millis();

  switch (find_mode::decideTimerWake(rtcState, powerManager.getBatteryPercentage())) {
    case find_mode::TimerWakeAction::NormalBoot:
      LOG_INF("FIND", "Timer wake without a usable state, booting");
      return;
    case find_mode::TimerWakeAction::SleepUntilButton:
      LOG_INF("FIND", "Battery below minimum, sleeping until the power button");
      powerManager.setWakeTimer(0);
      powerManager.startDeepSleep(gpio);
      return;
    case find_mode::TimerWakeAction::Scan:
      break;
  }

  buttonGpio = &gpio;
  find_mode::enterFastPath(rtcState);

  const find_mode::RadioResult heard = find_mode::listenForCode(rtcState.code, SCAN_WINDOW_MS, powerButtonPressed);
  if (heard == find_mode::RadioResult::ButtonPressed) {
    find_mode::radioOff();
    find_mode::leaveFastPath(rtcState, millis() - startedAt, false);
    LOG_INF("FIND", "Power button during scan, booting");
    return;
  }

  const bool heardCode = heard == find_mode::RadioResult::HeardCode;
  if (find_mode::afterScan(rtcState, heardCode) == find_mode::ScanOutcome::Announce) {
    LOG_INF("FIND", "Code heard, broadcasting %s", find_mode::FOUND_NAME);
    const find_mode::RadioResult found = find_mode::announceFound(FOUND_MODE_MS, powerButtonPressed);
    find_mode::radioOff();
    find_mode::leaveFastPath(rtcState, millis() - startedAt, true);
    if (found == find_mode::RadioResult::ButtonPressed) {
      find_mode::muteUntilCodeGone(rtcState);
      LOG_INF("FIND", "Found by the owner, booting");
      return;
    }
  } else {
    find_mode::radioOff();
    find_mode::leaveFastPath(rtcState, millis() - startedAt, heardCode);
  }

  powerManager.startDeepSleep(gpio);
}

void findModePrepareSleep(HalPowerManager& powerManager) {
  const FindSettings settings = currentSettings();
  if (!settings.enabled) {
    rtcState = {};
    armWakeTimer(powerManager);
    return;
  }

  // Keep counters, crash count and mute across sleeps for the same code.
  if (!find_mode::isStateValid(rtcState) || rtcState.code != settings.code) {
    rtcState = {};
    rtcState.code = settings.code;
  }
  rtcState.intervalMinutes = settings.intervalMinutes;
  rtcState.minBatteryPercent = settings.minBatteryPercent;
  find_mode::updateChecksum(rtcState);

  LOG_INF("FIND", "Armed: every %u min, %lu wakes, %lu detections, %lu ms awake", rtcState.intervalMinutes,
          static_cast<unsigned long>(rtcState.wakes), static_cast<unsigned long>(rtcState.detections),
          static_cast<unsigned long>(rtcState.awakeMs));
  armWakeTimer(powerManager);
}

#endif  // CROSSPOINT_FIND_MODE
