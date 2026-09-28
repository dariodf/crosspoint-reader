#include "FindModeActivity.h"

#if CROSSPOINT_FIND_MODE

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "FindModeRuntime.h"
#include "MappedInputManager.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "util/QrUtils.h"

namespace fui = freeink::ui;

namespace {
// The how-to row is a short paragraph; the other rows fit on one or two lines.
static constexpr int16_t MAX_ROW_LINES = 8;
// The QR takes at most this share of the screen height, leaving the rows room.
static constexpr int QR_HEIGHT_PERCENT = 35;
static constexpr int QR_SIDE_MARGIN = 20;

// One step up, back to the lowest after the highest.
uint8_t nextStep(const uint8_t value, const uint8_t lowest, const uint8_t highest, const uint8_t step) {
  return value + step > highest ? lowest : static_cast<uint8_t>(value + step);
}
}  // namespace

void FindModeActivity::onEnter() {
  // A clean full refresh, so the QR carries no ghost of the screen before.
  renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
  UiListActivity::onEnter();
  app.on(ACTION_SWITCH, &FindModeActivity::switchTrampoline, this);
  findModeEnsureCode();
  rows[ROW_CODE].label = SETTINGS.findModeCode;
  rows[ROW_HOW_TO].label = tr(STR_FIND_CODE_HINT);
  rows[ROW_TEST].label = tr(STR_FIND_TEST);
  rows[ROW_TEST].value = testValue;
  rows[ROW_INTERVAL].label = tr(STR_FIND_LISTEN_EVERY);
  rows[ROW_INTERVAL].value = intervalValue;
  rows[ROW_MIN_BATTERY].label = tr(STR_FIND_STOP_BELOW);
  rows[ROW_MIN_BATTERY].value = minBatteryValue;
  rows[ROW_NEW_CODE].label = tr(STR_FIND_NEW_CODE);
  offRow.label = tr(STR_FIND_OFF_HINT);
}

int FindModeActivity::firstShownRow() const {
  return findModeStatus() == FindModeStatus::SwitchedOff ? ROW_STATUS : ROW_CODE;
}

int FindModeActivity::listCount() const { return SETTINGS.findModeEnabled ? ROW_COUNT - firstShownRow() : 1; }

void FindModeActivity::switchTrampoline(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<FindModeActivity*>(user);
  SETTINGS.findModeEnabled = !SETTINGS.findModeEnabled;
  self->moveSelectionTo(0);
  self->requestUpdate();
}

void FindModeActivity::activateIndex(const int index) {
  if (!SETTINGS.findModeEnabled) return;
  nav.selected = index;
  switch (index + firstShownRow()) {
    case ROW_CODE:
      showQr();
      return;
    case ROW_TEST:
      app.clearTapFlash();
      findModeRequestTest();
      return;
    case ROW_STATUS:
      if (findModeStatus() == FindModeStatus::SwitchedOff) findModeRetryAfterSwitchOff();
      break;
    case ROW_INTERVAL:
      SETTINGS.findModeIntervalMinutes =
          nextStep(SETTINGS.findModeIntervalMinutes, CrossPointSettings::FIND_INTERVAL_MIN_MINUTES,
                   CrossPointSettings::FIND_INTERVAL_MAX_MINUTES, 1);
      break;
    case ROW_MIN_BATTERY:
      SETTINGS.findModeMinBatteryPercent = nextStep(
          SETTINGS.findModeMinBatteryPercent, CrossPointSettings::FIND_MIN_BATTERY_LOWEST_PERCENT,
          CrossPointSettings::FIND_MIN_BATTERY_HIGHEST_PERCENT, CrossPointSettings::FIND_MIN_BATTERY_STEP_PERCENT);
      break;
    case ROW_NEW_CODE:
      askForNewCode();
      return;
    default:
      return;
  }
  requestUpdate();
}

void FindModeActivity::showQr() {
  app.clearTapFlash();
  if (auto activity = makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, std::string(SETTINGS.findModeCode),
                                                           StrId::STR_FIND_CODE)) {
    startActivityForResult(std::move(activity), [this](const ActivityResult&) {
      renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
      requestUpdate();
    });
  } else {
    LOG_ERR("FIND", "OOM: QrDisplayActivity");
  }
}

void FindModeActivity::askForNewCode() {
  app.clearTapFlash();
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_FIND_NEW_CODE),
                                                         tr(STR_FIND_NEW_CODE_WARNING));
  if (!confirm) {
    LOG_ERR("FIND", "OOM: ConfirmationActivity");
    return;
  }
  startActivityForResult(std::move(confirm), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      // The render task reads the code for the rows; change it under the lock.
      RenderLock lock(*this);
      findModeNewCode();
    }
    renderer.promoteNextRefresh(HalDisplay::FULL_REFRESH);
    requestUpdate();
  });
}

void FindModeActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ToggleRowProps findSwitch;
  findSwitch.row.label = tr(STR_FIND_MODE);
  findSwitch.checked = SETTINGS.findModeEnabled;
  findSwitch.toggleAction = ACTION_SWITCH;
  screen.toggleRow(findSwitch);

  fui::ListProps props;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in the base loop
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = MAX_ROW_LINES;

  if (!SETTINGS.findModeEnabled) {
    qrSide = 0;
    props.items = &offRow;
    props.count = 1;
    syncListViewport(screen, props);
    props.selectedIndex = -1;  // a line of text, drawn plain
    screen.list(props);
    return;
  }

  qrTop = screen.body().y;
  qrSide =
      std::min(renderer.getScreenWidth() - 2 * QR_SIDE_MARGIN, renderer.getScreenHeight() * QR_HEIGHT_PERCENT / 100);
  screen.spacer(static_cast<int16_t>(qrSide + metrics.verticalSpacing));

  rows[ROW_STATUS].label = tr(STR_FIND_SWITCHED_OFF);
  int testRssi = 0;
  switch (findModeLastTest(testRssi)) {
    case FindModeTestResult::Heard:
      snprintf(testValue, sizeof(testValue), tr(STR_FIND_TEST_RESULT_HEARD_FORMAT), testRssi);
      break;
    case FindModeTestResult::NothingHeard:
      snprintf(testValue, sizeof(testValue), "%s", tr(STR_FIND_TEST_RESULT_NOTHING));
      break;
    case FindModeTestResult::Failed:
      snprintf(testValue, sizeof(testValue), "%s", tr(STR_FIND_TEST_RESULT_FAILED));
      break;
    case FindModeTestResult::Stopped:
      snprintf(testValue, sizeof(testValue), "%s", tr(STR_FIND_TEST_RESULT_STOPPED));
      break;
    case FindModeTestResult::None:
      testValue[0] = '\0';
      break;
  }
  snprintf(intervalValue, sizeof(intervalValue), tr(STR_SLEEP_TIMER_VALUE_FORMAT),
           static_cast<unsigned int>(SETTINGS.findModeIntervalMinutes));
  snprintf(minBatteryValue, sizeof(minBatteryValue), tr(STR_FIND_PERCENT_FORMAT),
           static_cast<unsigned int>(SETTINGS.findModeMinBatteryPercent));

  // Row actions carry the shown position, like button selection does.
  const int first = firstShownRow();
  for (int i = first; i < ROW_COUNT; i++) rows[i].actionValue = static_cast<int16_t>(i - first);
  props.items = rows + first;
  props.count = ROW_COUNT - first;
  syncListViewport(screen, props);
  screen.list(props);
}

void FindModeActivity::drawFooter() {
  if (qrSide > 0) {
    QrUtils::drawQrCode(renderer, Rect{0, qrTop, renderer.getScreenWidth(), qrSide}, SETTINGS.findModeCode);
  }
  UiListActivity::drawFooter();
}

#endif  // CROSSPOINT_FIND_MODE
