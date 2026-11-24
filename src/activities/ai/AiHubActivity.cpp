#include "AiHubActivity.h"

#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <memory>

#include "activities/ai/AiImageActivity.h"
#include "activities/voice/VoiceActivity.h"
#include "components/CompanionMascot.h"
#include "components/UITheme.h"
#include "fontIds.h"

void AiHubActivity::onEnter() {
  Activity::onEnter();
  // The base class does not trigger the first paint; without this the screen
  // keeps showing whatever the previous activity left in the framebuffer until
  // the first key press (ActivityManager only re-renders on request).
  requestUpdate();
}

void AiHubActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, ROW_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Push, not replace: Back from either front-end lands back here, so
    // switching between chat and image costs one press.
    if (selectedIndex == 0) {
      LOG_INF("AIHUB", "opening chat");
      activityManager.pushActivity(std::make_unique<VoiceActivity>(renderer, mappedInput));
    } else {
      LOG_INF("AIHUB", "opening image generation");
      activityManager.pushActivity(std::make_unique<AiImageActivity>(renderer, mappedInput));
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();  // hub replaced home, so this lands on the home screen
    return;
  }
}

void AiHubActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_VOICE));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  // Two rows under a tall hero would leave most of the panel blank, so the
  // buddy and the chooser are centred as one group. On a short landscape panel
  // the mascot is skipped and the list keeps the full height.
  const int captionH = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int heroH = CompanionMascot::SIZE + captionH;
  const int listH = ROW_COUNT * metrics.listRowHeight;
  const int groupH = heroH + metrics.verticalSpacing * 2 + listH;
  const int groupTop = contentTop + std::max(0, (contentHeight - groupH) / 2);
  const int heroUsed = CompanionMascot::drawBlock(renderer, screen.x, groupTop, screen.width, heroH,
                                                  CompanionMascot::Pose::Read, tr(STR_AI_HUB_TAGLINE));
  const int listTop = groupTop + (heroUsed > 0 ? heroUsed + metrics.verticalSpacing * 2 : 0);

  static constexpr StrId rowLabels[ROW_COUNT] = {StrId::STR_AI_CHAT, StrId::STR_AI_IMAGE};
  GUI.drawList(
      renderer, Rect{screen.x, listTop, screen.width, listH}, ROW_COUNT, selectedIndex,
      [](int index) { return I18N.get(rowLabels[index]); }, nullptr,
      [](int index) { return index == 0 ? Mic : Image; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
