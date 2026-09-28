#include "FindModeRuntime.h"

#if CROSSPOINT_FIND_MODE

#include <Arduino.h>
#include <FindRadio.h>
#include <FindState.h>
#include <HalSystem.h>
#include <Logging.h>
#include <WiFi.h>
#include <bootloader_random.h>
#include <esp_attr.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FindModeTestHooks.h"

namespace {

// How long each timer wake listens for the owner's code. At the phone's
// fastest advertising interval (~100 ms) this hears it about five times; the
// listen ends as soon as it does, so the full time is spent only on quiet wakes.
static constexpr uint32_t LISTEN_MS = 500;
// How long found mode broadcasts CP-FIND before sleeping again. Short, to save
// battery: while the phone keeps advertising, the next wake hears it again
// and starts another broadcast.
#ifdef CROSSPOINT_FIND_MODE_TEST_BROADCAST_SECONDS
// Test builds that collect detection timings cycle faster with a short broadcast.
static constexpr uint32_t BROADCAST_MS = CROSSPOINT_FIND_MODE_TEST_BROADCAST_SECONDS * 1000UL;
#else
static constexpr uint32_t BROADCAST_MS = 60UL * 1000UL;
#endif

// Time budgets for the fast-path guard below. Listening covers NimBLE start-up
// (normally ~0.3 s), the listen itself and the teardown.
static constexpr uint32_t LISTEN_BUDGET_MS = 5000;
// The broadcast budget also covers drawing the found screen (display start-up
// and one full refresh, a few seconds).
static constexpr uint32_t BROADCAST_BUDGET_MS = BROADCAST_MS + 10000;

// RTC_NOINIT keeps the state through deep sleep and through a crash reset, so
// countFastPathCrash() can see a fast path that died. After power loss it holds
// random bytes, which the checksum rejects.
RTC_NOINIT_ATTR find_mode::FindState sleepState;

bool settingsLoaded = false;

// FindRadio polls a plain function pointer, so the GPIO it reads lives here.
HalGPIO* gpioForButtonCheck = nullptr;

// Timer wakes set up only the power button (HalGPIO::beginPowerButtonOnly), so
// this reads it raw: down on two checks in a row (FindRadio checks every
// 10 ms) filters contact bounce, like the input manager's debounce.
bool powerButtonDownBefore = false;

bool powerButtonPressed() {
  if (findTestVirtualPress()) return true;
  const bool down = gpioForButtonCheck->isPowerButtonDown();
  const bool pressed = down && powerButtonDownBefore;
  powerButtonDownBefore = down;
  return pressed;
}

// The firmware's own panic check, plus brownout: a radio burst on a weak
// battery can pull the supply down mid-broadcast.
bool resetWasCrash() { return HalSystem::isRebootFromPanic() || esp_reset_reason() == ESP_RST_BROWNOUT; }

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
  // Both are clamped to their setting's range when settings.json loads.
  settings.intervalMinutes = SETTINGS.findModeIntervalMinutes;
  settings.minBatteryPercent = SETTINGS.findModeMinBatteryPercent;
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

uint32_t armedTimerSeconds() { return find_mode::isArmed(sleepState) ? sleepState.intervalMinutes * 60U : 0; }

// Every later sleep on this boot wakes on the timer too, while the mode is armed.
void armWakeTimer(HalPowerManager& powerManager) { powerManager.setWakeTimerSeconds(armedTimerSeconds()); }

}  // namespace

void findModeOnBoot(HalGPIO& gpio, HalPowerManager& powerManager) {
  // A brownout while the radio ran means the battery cannot carry it. A full
  // boot (SD card, screen refresh) would draw more on the same weak cell and
  // brown out again, so sleep until the owner presses power.
  findTestRecord(find_mode::JournalEvent::Boot, static_cast<uint8_t>(esp_reset_reason()),
                 HalGPIO::isTimerWake() ? 1 : 0);
  const bool brownedOutInFastPath =
      esp_reset_reason() == ESP_RST_BROWNOUT && find_mode::isStateValid(sleepState) && sleepState.inFastPath;
  find_mode::countFastPathCrash(sleepState, resetWasCrash());
  armWakeTimer(powerManager);
  if (brownedOutInFastPath) {
    LOG_ERR("FIND", "Brownout while the radio ran, sleeping until the power button");
    powerManager.setWakeTimerSeconds(0);
    powerManager.startDeepSleep(gpio);
  }
}

void findModeRunTimerWake(HalGPIO& gpio, HalPowerManager& powerManager, const FindModeFoundScreen showFoundScreen) {
  // The timer and the power button can fire together; the chip then reports
  // the timer. The owner pressed power, so hand over at once.
  if (esp_sleep_get_wakeup_causes() & (1U << ESP_SLEEP_WAKEUP_EXT1)) {
    LOG_INF("FIND", "Power button woke the reader with the timer, booting");
    return;
  }

  uint16_t battery = 0;
  if (!powerManager.readBatteryPercentage(battery)) {
    LOG_INF("FIND", "Battery gauge did not answer, listening anyway");
    battery = find_mode::BATTERY_UNKNOWN;
  }
  uint16_t injectedBattery = 0;
  const FindTestInjection injection = findTestTakeInjection(injectedBattery);
  if (injection == FindTestInjection::Battery) battery = injectedBattery;
  findTestRecord(find_mode::JournalEvent::Battery, 0, battery);

  switch (find_mode::decideTimerWake(sleepState, battery)) {
    case find_mode::TimerWakeAction::NormalBoot:
      LOG_INF("FIND", "Timer wake without a usable state, booting");
      return;
    case find_mode::TimerWakeAction::SleepUntilButton:
      LOG_INF("FIND", "Battery below minimum, sleeping until the power button");
      findTestRecord(find_mode::JournalEvent::Sleep, 1, 0);
      powerManager.setWakeTimerSeconds(0);
      powerManager.startDeepSleep(gpio);
      return;
    case find_mode::TimerWakeAction::Scan:
      break;
  }

  gpioForButtonCheck = &gpio;
  find_mode::enterFastPath(sleepState);
  armFastPathGuard(LISTEN_BUDGET_MS);
  if (injection == FindTestInjection::Crash) esp_system_abort("find mode test: injected crash");
  if (injection == FindTestInjection::Hang) {
    for (;;) delay(10);  // the guard aborts this after LISTEN_BUDGET_MS
  }

  bool ownerPressedPower = false;
  bool heardCode = false;
  const find_mode::RadioResult listenResult =
      injection == FindTestInjection::RadioFail
          ? find_mode::RadioResult::RadioFailed
          : find_mode::listenForCode(sleepState.code, LISTEN_MS, powerButtonPressed);
  findTestRecord(find_mode::JournalEvent::ListenStart, 0, find_mode::listenStartedAtMs());
  findTestRecord(find_mode::JournalEvent::ListenEnd, static_cast<uint8_t>(listenResult), millis());
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
        // Tells whoever holds the reader what is going on. Drawn once: e-ink
        // keeps it through every later sleep until the next sleep screen.
        if (find_mode::needsFoundScreen(sleepState)) {
          showFoundScreen(sleepState.language, sleepState.orientation);
          find_mode::markFoundScreenShown(sleepState);
          findTestRecord(find_mode::JournalEvent::FoundScreen, 0, millis());
        }
        const uint32_t broadcastStartedAt = millis();
        const find_mode::RadioResult broadcastResult = find_mode::broadcastFound(BROADCAST_MS, powerButtonPressed);
        findTestRecord(find_mode::JournalEvent::BroadcastEnd, static_cast<uint8_t>(broadcastResult),
                       millis() - broadcastStartedAt);
        if (broadcastResult == find_mode::RadioResult::ButtonPressed) {
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
  // A press during start-up or teardown missed the radio's checks. The sleep
  // path would wait for its release and sleep anyway, so look once more.
  if (ownerPressedPower || gpio.isPowerButtonDown()) {
    findTestRecord(find_mode::JournalEvent::Handover, 0, 0);
    return;
  }

  // The wake timer armed by findModeOnBoot() is still set for this sleep.
  findTestRecord(find_mode::JournalEvent::Sleep, 1, armedTimerSeconds());
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
  sleepState.language = SETTINGS.language;
  // Same rule as the sleep screen's popup: the reading orientation when the
  // reader went to sleep from a book, portrait otherwise.
  sleepState.orientation =
      APP_STATE.lastSleepFromReader ? SETTINGS.orientation : static_cast<uint8_t>(CrossPointSettings::PORTRAIT);
  // This sleep draws the sleep screen over any found screen.
  sleepState.foundScreenShown = 0;
  find_mode::updateChecksum(sleepState);

  LOG_INF("FIND", "Armed: every %u min, %lu wakes, %lu detections, %lu ms awake", sleepState.intervalMinutes,
          static_cast<unsigned long>(sleepState.wakes), static_cast<unsigned long>(sleepState.detections),
          static_cast<unsigned long>(sleepState.awakeMs));
  armWakeTimer(powerManager);
  findTestRecord(find_mode::JournalEvent::Sleep, 0, armedTimerSeconds());
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

#if CROSSPOINT_FIND_MODE_TEST_HOOKS
void findModePrintState() {
  const find_mode::FindState& s = sleepState;
  logSerial.printf(
      "FIND_STATE valid=%d armed=%d wakes=%lu detections=%lu awakeMs=%lu crashes=%u switchedOff=%u muted=%u "
      "inFastPath=%u interval=%u minBattery=%u language=%u orientation=%u foundScreen=%u\n",
      find_mode::isStateValid(s), find_mode::isArmed(s), static_cast<unsigned long>(s.wakes),
      static_cast<unsigned long>(s.detections), static_cast<unsigned long>(s.awakeMs), s.fastPathCrashes,
      s.switchedOffByCrashes, s.mutedUntilCodeGone, s.inFastPath, s.intervalMinutes, s.minBatteryPercent, s.language,
      s.orientation, s.foundScreenShown);
}
#endif

#endif  // CROSSPOINT_FIND_MODE
