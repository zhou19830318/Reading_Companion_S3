#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>

#include "components/UITheme.h"
#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (renderer.getScreenHeight() - height) / 2;

  renderer.clearScreen();
  if (!GUI.drawSystemMessage(renderer, UITheme::getInstance().getScreenSafeArea(renderer), text.c_str(), nullptr)) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, text.c_str(), true, style);
  }
  renderer.displayBuffer(refreshMode);
}
