#pragma once

// Settings > System > BLE Find Mode. The switch sits at the top; with find
// mode on, the code's QR follows, so a phone copies it in one scan, and below
// it a list that touch and buttons both work:
//
//   Stopped after crashes  only after the mode switched itself off; select
//                       to retry
//   <the code>          select to show the QR full screen
//   <how-to>            where the code goes in nRF Connect
//   Test mode           restarts into a one-minute listen, then echoes CP-FIND;
//                       shows the last result
//   Listen every        1 to 5 minutes; select to step
//   Stop below battery  10, 15 or 20 %; select to step
//   New code            asks first: a phone set up with the old code stops
//                       finding the reader
//
// With find mode off, the list holds one line on what it does.
// Opening the screen creates a code when there is none (or it is not valid).

#include <I18n.h>

#include "activities/UiListActivity.h"

class FindModeActivity final : public UiListActivity {
 public:
  explicit FindModeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("FindMode", renderer, mappedInput) {}

  void onEnter() override;

 private:
  static constexpr freeink::ui::ActionId ACTION_SWITCH = ACTION_USER;

  enum Row { ROW_STATUS, ROW_CODE, ROW_HOW_TO, ROW_TEST, ROW_INTERVAL, ROW_MIN_BATTERY, ROW_NEW_CODE, ROW_COUNT };

  int listCount() const override;
  // Rows before this one stay hidden: the status row shows only when there is
  // a switch-off to retry.
  int firstShownRow() const;
  const char* headerTitle() const override { return tr(STR_FIND_MODE); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void drawFooter() override;

  static void switchTrampoline(const freeink::ui::ActionEvent& event, void* user);
  void showQr();
  void askForNewCode();

  // Row text points into these buffers and translation strings, so a render
  // allocates nothing.
  char testValue[32] = {0};
  char intervalValue[16] = {0};
  char minBatteryValue[16] = {0};
  freeink::ui::ListItem rows[ROW_COUNT]{};
  freeink::ui::ListItem offRow{};
  // Where buildScreen left room for the QR; drawFooter draws it there.
  int qrTop = 0;
  int qrSide = 0;
};
