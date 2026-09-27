#include "FindModeRuntime.h"

#if CROSSPOINT_FIND_MODE

#include <Arduino.h>
#include <FindRadio.h>
#include <FindState.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace {

// How long each timer wake listens for the owner's code. The phone advertises
// every ~100 ms, so a full second hears it several times over.
static constexpr uint32_t LISTEN_MS = 1000;
// How long found mode broadcasts CP-FIND before giving up and sleeping again.
static constexpr uint32_t BROADCAST_MS = 5UL * 60UL * 1000UL;

// RTC_NOINIT keeps the state through deep sleep and through a crash reset, so
// countFastPathCrash() can see a fast path that died. After power loss it holds
// random bytes, which the checksum rejects.
RTC_NOINIT_ATTR find_mode::FindState sleepState;

// FindRadio polls a plain function pointer, so the GPIO it reads lives here.
HalGPIO* gpioForButtonCheck = nullptr;

bool powerButtonPressed() {
  gpioForButtonCheck->update();
  return gpioForButtonCheck->isPressed(HalGPIO::BTN_POWER);
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

// The find-mode settings. Test builds read them from build flags until the
// settings screen exists; without those flags the mode is off.
struct FindSettings {
  bool enabled;
  uint8_t intervalMinutes;
  uint8_t minBatteryPercent;
  find_mode::Code code;
};

FindSettings readSettings() {
  FindSettings settings{};
#ifdef CROSSPOINT_FIND_MODE_TEST_CODE
  settings.enabled = find_mode::parseCode(CROSSPOINT_FIND_MODE_TEST_CODE, settings.code);
  settings.intervalMinutes = CROSSPOINT_FIND_MODE_TEST_INTERVAL_MINUTES;
  settings.minBatteryPercent = CROSSPOINT_FIND_MODE_TEST_MIN_BATTERY;
#endif
  return settings;
}

// Every later sleep on this boot wakes on the timer too, as long as the state
// is usable and the mode has not switched itself off.
void armWakeTimer(HalPowerManager& powerManager) {
  const bool armed = find_mode::isStateValid(sleepState) && !sleepState.switchedOffByCrashes;
  powerManager.setWakeTimerSeconds(armed ? sleepState.intervalMinutes * 60U : 0);
}

}  // namespace

void findModeOnBoot(HalPowerManager& powerManager) {
  find_mode::countFastPathCrash(sleepState, resetWasCrash());
  armWakeTimer(powerManager);
}

void findModeRunTimerWake(HalGPIO& gpio, HalPowerManager& powerManager) {
  const uint32_t startedAt = millis();

  switch (find_mode::decideTimerWake(sleepState, powerManager.getBatteryPercentage())) {
    case find_mode::TimerWakeAction::NormalBoot:
      LOG_INF("FIND", "Timer wake without a usable state, booting");
      return;
    case find_mode::TimerWakeAction::SleepUntilButton:
      LOG_INF("FIND", "Battery below minimum, sleeping until the power button");
      powerManager.setWakeTimerSeconds(0);
      powerManager.startDeepSleep(gpio);
      return;
    case find_mode::TimerWakeAction::Scan:
      break;
  }

  gpioForButtonCheck = &gpio;
  find_mode::enterFastPath(sleepState);

  const find_mode::RadioResult listenResult = find_mode::listenForCode(sleepState.code, LISTEN_MS, powerButtonPressed);
  if (listenResult == find_mode::RadioResult::ButtonPressed) {
    find_mode::stopRadio();
    find_mode::leaveFastPath(sleepState, millis() - startedAt, false);
    LOG_INF("FIND", "Power button while listening, booting");
    return;
  }

  const bool heardCode = listenResult == find_mode::RadioResult::HeardCode;
  if (find_mode::decideAfterScan(sleepState, heardCode) == find_mode::AfterScanAction::BroadcastFound) {
    LOG_INF("FIND", "Code heard, broadcasting %s", find_mode::FOUND_NAME);
    const find_mode::RadioResult broadcastResult = find_mode::broadcastFound(BROADCAST_MS, powerButtonPressed);
    find_mode::stopRadio();
    find_mode::leaveFastPath(sleepState, millis() - startedAt, true);
    if (broadcastResult == find_mode::RadioResult::ButtonPressed) {
      // The owner has the reader. Their phone may still be advertising, so the
      // next wakes ignore the code until one wake no longer hears it.
      find_mode::muteUntilCodeGone(sleepState);
      LOG_INF("FIND", "Found by the owner, booting");
      return;
    }
  } else {
    find_mode::stopRadio();
    find_mode::leaveFastPath(sleepState, millis() - startedAt, heardCode);
  }

  // The wake timer armed by findModeOnBoot() is still set for this sleep.
  powerManager.startDeepSleep(gpio);
}

void findModePrepareSleep(HalPowerManager& powerManager) {
  const FindSettings settings = readSettings();
  if (!settings.enabled) {
    sleepState = {};
    armWakeTimer(powerManager);
    return;
  }

  // Keep counters, crash count and mute across sleeps while the code stays the
  // same; a new code starts from a clean state.
  if (!find_mode::isStateValid(sleepState) || sleepState.code != settings.code) {
    sleepState = {};
    sleepState.code = settings.code;
  }
  sleepState.intervalMinutes = settings.intervalMinutes;
  sleepState.minBatteryPercent = settings.minBatteryPercent;
  find_mode::updateChecksum(sleepState);

  LOG_INF("FIND", "Armed: every %u min, %lu wakes, %lu detections, %lu ms awake", sleepState.intervalMinutes,
          static_cast<unsigned long>(sleepState.wakes), static_cast<unsigned long>(sleepState.detections),
          static_cast<unsigned long>(sleepState.awakeMs));
  armWakeTimer(powerManager);
}

#endif  // CROSSPOINT_FIND_MODE
