#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>

#include "fontIds.h"
#include "images/BootAnimationFrames.h"

namespace {
constexpr int kImageY = 170;
constexpr int kImageWidth = 440;
constexpr unsigned long kFrameDurations[] = {200, 200, 200, 1600};
constexpr size_t kFrameCount = sizeof(kFrameDurations) / sizeof(kFrameDurations[0]);
const uint8_t* kFrames[] = {BootFrameLeft, BootFrameCenter, BootFrameRight, BootFrameSettle};
}  // namespace

void BootActivity::onEnter() {
  Activity::onEnter();

  frameIndex = 0;
  animationComplete = false;
  requestUpdateAndWait();
  nextFrameMs = millis() + kFrameDurations[frameIndex];
}

void BootActivity::loop() {
  if (animationComplete || static_cast<long>(millis() - nextFrameMs) < 0) {
    return;
  }

  if (frameIndex + 1 >= kFrameCount) {
    animationComplete = true;
    return;
  }

  frameIndex++;
  requestUpdateAndWait();
  nextFrameMs = millis() + kFrameDurations[frameIndex];
}

void BootActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(kFrames[frameIndex], (pageWidth - kImageWidth) / 2, kImageY, kBootFrameWidth, kBootFrameHeight);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 65, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);

  std::string booting = tr(STR_BOOTING);
  booting.append(frameIndex + 1, '.');
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 125, booting.c_str());
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, CROSSPOINT_VERSION);

  renderer.displayBuffer(frameIndex == 0 ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
}
