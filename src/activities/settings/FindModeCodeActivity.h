#pragma once

// Settings > System > Find mode code: the secret the owner's phone advertises
// to find this reader. A list, so touch and buttons both work:
//
//   Find mode      Listening / Off / Stopped after crashes (select to retry)
//   <the code>     select to show it as a QR
//   Show as QR     scan it with the phone to copy the text into nRF Connect
//   <counters>     wakes, detections, seconds awake since the code changed
//   New code       asks first: a phone set up with the old code stops finding
//                  the reader
//   <how-to>       where the code goes in nRF Connect
//
// Opening the screen creates a code when there is none (or it is not valid).

#include <I18n.h>

#include "activities/UiListActivity.h"

class FindModeCodeActivity final : public UiListActivity {
 public:
  explicit FindModeCodeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("FindModeCode", renderer, mappedInput) {}

  void onEnter() override;

 private:
  enum Row { ROW_STATUS, ROW_CODE, ROW_SHOW_QR, ROW_STATS, ROW_NEW_CODE, ROW_HOW_TO, ROW_COUNT };

  int listCount() const override { return ROW_COUNT; }
  const char* headerTitle() const override { return tr(STR_FIND_CODE); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;

  void showQr();
  void askForNewCode();

  // Row text points into these buffers and translation strings, so a render
  // allocates nothing.
  char statsLine[96] = {0};
  freeink::ui::ListItem rows[ROW_COUNT]{};
};
