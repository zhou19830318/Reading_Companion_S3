#include "CompanionMascot.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "components/icons/mascot_chat128.h"
#include "components/icons/mascot_listen128.h"
#include "components/icons/mascot_lookup128.h"
#include "components/icons/mascot_paint128.h"
#include "components/icons/mascot_read128.h"
#include "components/icons/mascot_sad128.h"
#include "components/icons/mascot_think128.h"
#include "fontIds.h"

namespace CompanionMascot {

namespace {

const uint8_t* iconFor(const Pose pose) {
  switch (pose) {
    case Pose::Read:
      return MascotReadIcon;
    case Pose::Chat:
      return MascotChatIcon;
    case Pose::Listen:
      return MascotListenIcon;
    case Pose::Lookup:
      return MascotLookupIcon;
    case Pose::Paint:
      return MascotPaintIcon;
    case Pose::Think:
      return MascotThinkIcon;
    case Pose::Sad:
      return MascotSadIcon;
  }
  return MascotReadIcon;
}

}  // namespace

bool draw(const GfxRenderer& renderer, const int x, const int y, const Pose pose) {
  if (x < 0 || y < 0 || y + SIZE > renderer.getScreenHeight() || x + SIZE > renderer.getScreenWidth()) return false;
  renderer.drawIcon(iconFor(pose), x, y & ~7, SIZE, SIZE);
  return true;
}

bool drawCentered(const GfxRenderer& renderer, const int left, const int width, const int y, const Pose pose) {
  if (width < SIZE) return false;
  return draw(renderer, left + (width - SIZE) / 2, y, pose);
}

int drawBlock(const GfxRenderer& renderer, const int left, const int top, const int width, const int height,
              const Pose pose, const char* caption) {
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const bool hasCaption = caption != nullptr && caption[0] != '\0';
  const int captionH = hasCaption ? lineH + 8 : 0;
  if (width < SIZE || height < SIZE + captionH) return 0;
  if (top < 0 || top + SIZE + captionH > renderer.getScreenHeight()) return 0;

  // Centre the icon + caption group; spare room goes above and below equally.
  const int groupH = SIZE + captionH;
  const int y = top + (height - groupH) / 2;
  if (!drawCentered(renderer, left, width, y, pose)) return 0;
  if (hasCaption) {
    std::string text(caption);
    if (renderer.getTextWidth(UI_10_FONT_ID, text.c_str()) > width) {
      text.resize(renderer.fitPrefixLen(UI_10_FONT_ID, text, width));
    }
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
    renderer.drawText(UI_10_FONT_ID, left + std::max(0, (width - textW) / 2), y + SIZE + 8, text.c_str());
  }
  return groupH;
}

}  // namespace CompanionMascot
