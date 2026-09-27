#include "FindModeRuntime.h"

#if CROSSPOINT_FIND_MODE

#include <Arduino.h>
#include <FindRadio.h>
#include <FindState.h>
#include <Logging.h>
#include <WiFi.h>
#include <bootloader_random.h>
#include <esp_attr.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "CrossPointSettings.h"

namespace {

// How long each timer wake listens for the owner's code. At the phone's
// fastest advertising interval (~100 ms) this hears it about five times; the
// listen ends as soon as it does, so the full time is spent only on quiet wakes.
static constexpr uint32_t LISTEN_MS = 500;
// How long found mode broadcasts CP-FIND before giving up and sleeping again.
static constexpr uint32_t BROADCAST_MS = 5UL * 60UL * 1000UL;

// Time budgets for the fast-path guard below. Listening covers NimBLE start-up
// (normally ~0.3 s), the listen itself and the teardown.
static constexpr uint32_t LISTEN_BUDGET_MS = 5000;
static constexpr uint32_t BROADCAST_BUDGET_MS = BROADCAST_MS + 5000;

// RTC_NOINIT keeps the state through deep sleep and through a crash reset, so
// countFastPathCrash() can see a fast path that died. After power loss it holds
// random bytes, which the checksum rejects.
RTC_NOINIT_ATTR find_mode::FindState sleepState;

bool settingsLoaded = false;

// FindRadio polls a plain function pointer, so the GPIO it reads lives here.
HalGPIO* gpioForButtonCheck = nullptr;

bool powerButtonPressed() {
  gpioForButtonCheck->update();
  return gpioForButtonCheck->isPressed(HalGPIO::BTN_POWER);
}

bool resetWasCrash() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:  // a radio burst on a weak battery
      return true;
    default:
      return false;
  }
}

// The fast-path guard. The Arduino loop task has no watchdog, and a NimBLE call
// that never returns (init() waits for the controller to sync with no timeout)
// would keep the reader awake with the radio on until the battery died. A
// one-shot timer aborts instead: the panic reset counts as a fast-path crash,
// so three of them switch the mode off.
esp_timer_handle_t fastPathGuard = nullptr;

void onFastPathOverrun(void*) { esp_system_abort("find mode: fast path overran its time budget"); }

void armFastPathGuard(const uint32_t budgetMs) {
  if (fastPathGuard == nullptr) {
    esp_timer_create_args_t args{};
    args.callback = onFastPathOverrun;
    args.name = "findGuard";
    if (esp_timer_create(&args, &fastPathGuard) != ESP_OK) return;
  }
  esp_timer_stop(fastPathGuard);
  esp_timer_start_once(fastPathGuard, uint64_t{budgetMs} * 1000ULL);
}

void disarmFastPathGuard() {
  if (fastPathGuard != nullptr) esp_timer_stop(fastPathGuard);
}

// The find-mode settings, from Settings > System. Builds with
// CROSSPOINT_FIND_MODE_TEST_CODE use build flags instead, for test boards with
// no SD card to hold settings.json.
struct FindSettings {
  bool enabled;
  uint8_t intervalMinutes;
  uint8_t minBatteryPercent;
  find_mode::Code code;
};

// Settings store an index into a choices table. fromJson and the web page
// already keep it in range; this keeps a bad index from reading past the table.
template <size_t N>
uint8_t choiceOrLast(const uint8_t (&choices)[N], const uint8_t index) {
  return choices[index < N ? index : N - 1];
}

FindSettings readSettings() {
  FindSettings settings{};
#ifdef CROSSPOINT_FIND_MODE_TEST_CODE
  settings.enabled = find_mode::parseCode(CROSSPOINT_FIND_MODE_TEST_CODE, settings.code);
  settings.intervalMinutes = CROSSPOINT_FIND_MODE_TEST_INTERVAL_MINUTES;
  settings.minBatteryPercent = CROSSPOINT_FIND_MODE_TEST_MIN_BATTERY;
#else
  // With no valid code there is nothing a phone could send, so the mode stays
  // off until the Find mode code screen has created one.
  settings.enabled = SETTINGS.findModeEnabled && find_mode::parseCode(SETTINGS.findModeCode, settings.code);
  settings.intervalMinutes = choiceOrLast(CrossPointSettings::FIND_INTERVAL_MINUTES, SETTINGS.findModeInterval);
  settings.minBatteryPercent = choiceOrLast(CrossPointSettings::FIND_MIN_BATTERY_PERCENT, SETTINGS.findModeMinBattery);
#endif
  return settings;
}

bool settingsAvailable() {
#ifdef CROSSPOINT_FIND_MODE_TEST_CODE
  return true;
#else
  return settingsLoaded;
#endif
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
  uint16_t battery = 0;
  if (!powerManager.readBatteryPercentage(battery)) {
    LOG_INF("FIND", "Battery gauge did not answer, listening anyway");
    battery = find_mode::BATTERY_UNKNOWN;
  }

  switch (find_mode::decideTimerWake(sleepState, battery)) {
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
  armFastPathGuard(LISTEN_BUDGET_MS);

  bool ownerPressedPower = false;
  bool heardCode = false;
  const find_mode::RadioResult listenResult = find_mode::listenForCode(sleepState.code, LISTEN_MS, powerButtonPressed);
  switch (listenResult) {
    case find_mode::RadioResult::ButtonPressed:
      LOG_INF("FIND", "Power button while listening, booting");
      ownerPressedPower = true;
      break;
    case find_mode::RadioResult::RadioFailed:
      // Counts as neither heard nor quiet: a quiet scan would clear the mute.
      LOG_ERR("FIND", "Radio failed to start");
      break;
    case find_mode::RadioResult::HeardCode:
    case find_mode::RadioResult::NothingHeard:
    case find_mode::RadioResult::TimeUp:
      heardCode = listenResult == find_mode::RadioResult::HeardCode;
      if (find_mode::decideAfterScan(sleepState, heardCode) == find_mode::AfterScanAction::BroadcastFound) {
        LOG_INF("FIND", "Code heard, broadcasting %s", find_mode::FOUND_NAME);
        armFastPathGuard(BROADCAST_BUDGET_MS);
        if (find_mode::broadcastFound(BROADCAST_MS, powerButtonPressed) == find_mode::RadioResult::ButtonPressed) {
          // The owner has the reader. Their phone may still be advertising, so
          // the next wakes ignore the code until one wake no longer hears it.
          find_mode::muteUntilCodeGone(sleepState);
          LOG_INF("FIND", "Found by the owner, booting");
          ownerPressedPower = true;
        }
      }
      break;
  }

  find_mode::stopRadio();
  disarmFastPathGuard();
  // millis() counts from boot, so this includes the start-up before listening.
  find_mode::leaveFastPath(sleepState, millis(), heardCode);
  if (ownerPressedPower) return;

  // The wake timer armed by findModeOnBoot() is still set for this sleep.
  powerManager.startDeepSleep(gpio);
}

void findModeSettingsLoaded() { settingsLoaded = true; }

void findModePrepareSleep(HalPowerManager& powerManager) {
  if (!settingsAvailable()) {
    armWakeTimer(powerManager);
    return;
  }

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

FindModeStatus findModeStatus() {
  if (find_mode::isStateValid(sleepState) && sleepState.switchedOffByCrashes) return FindModeStatus::SwitchedOff;
  return readSettings().enabled ? FindModeStatus::Listening : FindModeStatus::Off;
}

FindModeStats findModeStats() {
  if (!find_mode::isStateValid(sleepState)) return {};
  return {sleepState.wakes, sleepState.detections, sleepState.awakeMs};
}

void findModeEnsureCode() {
  find_mode::Code code;
  if (!find_mode::parseCode(SETTINGS.findModeCode, code)) findModeNewCode();
}

void findModeNewCode() {
  find_mode::Code code;
  // The RNG is truly random only while an entropy source runs. With Wi-Fi off
  // (Bluetooth is always off outside the fast path), switch on the bootloader's
  // source, the SAR ADC, which this board leaves unused.
  const bool wifiOff = WiFi.getMode() == WIFI_OFF;
  if (wifiOff) bootloader_random_enable();
  esp_fill_random(code.data(), code.size());
  if (wifiOff) bootloader_random_disable();

  find_mode::formatCode(code, SETTINGS.findModeCode);
  SETTINGS.saveToFile();
  // The counters describe the old code; the next sleep rebuilds the state.
  sleepState = {};
  LOG_INF("FIND", "New code created");
}

void findModeRetryAfterSwitchOff() {
  if (find_mode::isStateValid(sleepState)) find_mode::retryAfterSwitchOff(sleepState);
}

#endif  // CROSSPOINT_FIND_MODE
