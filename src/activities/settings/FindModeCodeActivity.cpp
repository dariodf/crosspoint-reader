#include "FindModeCodeActivity.h"

#if CROSSPOINT_FIND_MODE

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "FindModeRuntime.h"
#include "MappedInputManager.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// The how-to row is a short paragraph; the other rows fit on one or two lines.
static constexpr int16_t MAX_ROW_LINES = 6;
}  // namespace

void FindModeCodeActivity::onEnter() {
  UiListActivity::onEnter();
  findModeEnsureCode();
  for (int i = 0; i < ROW_COUNT; i++) rows[i].actionValue = static_cast<int16_t>(i);
  rows[ROW_STATUS].label = tr(STR_FIND_MODE);
  rows[ROW_CODE].label = SETTINGS.findModeCode;
  rows[ROW_SHOW_QR].label = tr(STR_FIND_SHOW_QR);
  rows[ROW_STATS].label = statsLine;
  rows[ROW_NEW_CODE].label = tr(STR_FIND_NEW_CODE);
  rows[ROW_HOW_TO].label = tr(STR_FIND_CODE_HINT);
}

void FindModeCodeActivity::activateIndex(const int index) {
  nav.selected = index;
  switch (index) {
    case ROW_STATUS:
      if (findModeStatus() == FindModeStatus::SwitchedOff) findModeRetryAfterSwitchOff();
      requestUpdate();
      return;
    case ROW_CODE:
    case ROW_SHOW_QR:
      showQr();
      return;
    case ROW_NEW_CODE:
      askForNewCode();
      return;
    default:
      return;
  }
}

void FindModeCodeActivity::showQr() {
  app.clearTapFlash();
  if (auto activity = makeUniqueNoThrow<QrDisplayActivity>(renderer, mappedInput, std::string(SETTINGS.findModeCode),
                                                           StrId::STR_FIND_CODE)) {
    startActivityForResult(std::move(activity), nullptr);
  } else {
    LOG_ERR("FIND", "OOM: QrDisplayActivity");
  }
}

void FindModeCodeActivity::askForNewCode() {
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
    requestUpdate();
  });
}

void FindModeCodeActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  switch (findModeStatus()) {
    case FindModeStatus::Listening:
      rows[ROW_STATUS].value = tr(STR_FIND_LISTENING);
      break;
    case FindModeStatus::Off:
      rows[ROW_STATUS].value = tr(STR_STATE_OFF);
      break;
    case FindModeStatus::SwitchedOff:
      rows[ROW_STATUS].value = tr(STR_FIND_SWITCHED_OFF);
      break;
  }
  const FindModeStats stats = findModeStats();
  snprintf(statsLine, sizeof(statsLine), tr(STR_FIND_STATS), static_cast<unsigned long>(stats.wakes),
           static_cast<unsigned long>(stats.detections), static_cast<unsigned long>(stats.awakeMs / 1000));

  fui::ListProps props;
  props.items = rows;
  props.count = ROW_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in the base loop
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = MAX_ROW_LINES;
  syncListViewport(screen, props);
  screen.list(props);
}

#endif  // CROSSPOINT_FIND_MODE
