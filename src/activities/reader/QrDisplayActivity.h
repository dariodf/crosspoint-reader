#pragma once
#include <I18n.h>

#include <string>

#include "activities/Activity.h"

class QrDisplayActivity final : public Activity {
 public:
  explicit QrDisplayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& textPayload,
                             StrId title = StrId::STR_DISPLAY_QR)
      : Activity("QrDisplay", renderer, mappedInput), textPayload(textPayload), title(title) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string textPayload;
  StrId title;
};
