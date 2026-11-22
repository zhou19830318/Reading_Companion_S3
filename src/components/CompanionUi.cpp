#include "CompanionUi.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "fontIds.h"

namespace CompanionUi {

int drawSectionLabel(const GfxRenderer& renderer, const int x, const int y, const int width, const char* label) {
  if (label == nullptr || label[0] == '\0' || width <= 0) return 0;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x, y, label, true, EpdFontFamily::BOLD);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
  const int ruleX = x + labelW + 10;
  const int ruleY = y + lineH / 2;
  if (ruleX < x + width) {
    renderer.drawLine(ruleX, ruleY, x + width - 1, ruleY, true);
  }
  return lineH;
}

void drawSelectRow(const GfxRenderer& renderer, const Rect rect, const int index, const char* title,
                   const char* trailing, const bool selected) {
  if (title == nullptr || rect.width <= 0 || rect.height <= 0) return;

  constexpr int pad = 8;
  constexpr int corner = 6;
  constexpr int boxGap = 10;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  if (selected) {
    renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, corner, Color::LightGray);
    // Leading notch: reads as a bookmark tab rather than a generic highlight.
    const int notchH = std::max(10, rect.height - 14);
    renderer.fillRect(rect.x, rect.y + (rect.height - notchH) / 2, 3, notchH, true);
  } else {
    renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
  }

  const int box = std::min(26, std::max(16, rect.height - 12));
  const int boxX = rect.x + pad + 3;
  const int boxY = rect.y + (rect.height - box) / 2;
  char indexText[8];
  snprintf(indexText, sizeof(indexText), "%02d", std::max(1, index));
  const int indexW = renderer.getTextWidth(UI_10_FONT_ID, indexText, EpdFontFamily::BOLD);

  if (selected) {
    renderer.fillRoundedRect(boxX, boxY, box, box, 4, Color::Black);
    renderer.drawText(UI_10_FONT_ID, boxX + (box - indexW) / 2, boxY + (box - lineH) / 2, indexText, false,
                      EpdFontFamily::BOLD);
  } else {
    renderer.drawRoundedRect(boxX, boxY, box, box, 1, 4, true);
    renderer.drawText(UI_10_FONT_ID, boxX + (box - indexW) / 2, boxY + (box - lineH) / 2, indexText, true,
                      EpdFontFamily::BOLD);
  }

  const int titleX = boxX + box + boxGap;
  const int trailingW =
      (trailing != nullptr && trailing[0] != '\0') ? renderer.getTextWidth(UI_10_FONT_ID, trailing) + 8 : 0;
  const int titleAvail = rect.x + rect.width - pad - trailingW - titleX;
  std::string titleText(title);
  if (titleAvail > 0 && renderer.getTextWidth(UI_10_FONT_ID, titleText.c_str()) > titleAvail) {
    titleText.resize(renderer.fitPrefixLen(UI_10_FONT_ID, titleText, titleAvail));
  }
  const int titleY = rect.y + (rect.height - lineH) / 2;
  renderer.drawText(UI_10_FONT_ID, titleX, titleY, titleText.c_str(), true,
                    selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  if (trailingW > 0) {
    const int trailingX = rect.x + rect.width - pad - renderer.getTextWidth(UI_10_FONT_ID, trailing);
    renderer.drawText(UI_10_FONT_ID, trailingX, titleY, trailing, true);
  }
}

int drawChip(const GfxRenderer& renderer, const int right, const int y, const char* text, const bool filled) {
  if (text == nullptr || text[0] == '\0') return 0;
  constexpr int padX = 7;
  constexpr int padY = 3;
  const int lineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int w = renderer.getTextWidth(SMALL_FONT_ID, text) + padX * 2;
  const int h = lineH + padY * 2;
  if (filled) {
    renderer.fillRoundedRect(right - w, y, w, h, h / 2, Color::Black);
    renderer.drawText(SMALL_FONT_ID, right - w + padX, y + padY, text, false);
  } else {
    renderer.drawRoundedRect(right - w, y, w, h, 1, h / 2, true);
    renderer.drawText(SMALL_FONT_ID, right - w + padX, y + padY, text, true);
  }
  return w;
}

void drawStepDots(const GfxRenderer& renderer, const int right, const int y, const int count, const int current) {
  if (count <= 0) return;
  constexpr int dot = 7;
  constexpr int gap = 6;
  const int total = count * dot + (count - 1) * gap;
  int x = right - total;
  for (int i = 0; i < count; ++i) {
    if (i == current) {
      renderer.fillRoundedRect(x, y, dot, dot, dot / 2, Color::Black);
    } else {
      renderer.drawRoundedRect(x, y, dot, dot, 1, dot / 2, true);
    }
    x += dot + gap;
  }
}

}  // namespace CompanionUi
