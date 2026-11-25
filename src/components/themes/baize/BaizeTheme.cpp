#include "BaizeTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/mascot_read128.h"
#include "fontIds.h"

namespace {
// The complete reading-companion drawing already ships for the AI screens.
// Reusing it keeps the home banner upright and whole instead of the old crop.
constexpr int SPRITE_W = 128;
constexpr int SPRITE_H = 128;
// Below the battery row so the two never draw over each other.
constexpr int spriteTop = 20;
constexpr int textLeftPadding = 24;
constexpr int spriteRightGap = 8;
constexpr int textSpriteGap = 12;
constexpr int cardRadius = 14;
constexpr int inset = 20;

// Decode the existing flash sprite directly, avoiding scaled bitmap buffers.
// The source is stored rotated CCW with 1 = paper.
void companion(const GfxRenderer& renderer, int x, int y, int width) {
  const int height = width * SPRITE_H / SPRITE_W;
  for (int dy = 0; dy < height; ++dy) {
    const int sy = dy * SPRITE_H / height;
    for (int dx = 0; dx < width; ++dx) {
      const int sx = dx * SPRITE_W / width;
      const int offset = (SPRITE_W - 1 - sx) * (SPRITE_H / 8) + sy / 8;
      if (!(MascotReadIcon[offset] & (0x80 >> (sy % 8)))) renderer.drawPixel(x + dx, y + dy, true);
    }
  }
}

// Bounded UTF-8 wrapping for UI labels. No temporary vectors or ellipsis.
// A continuation chevron signals genuine overflow, never an unknown value.
int textBlock(const GfxRenderer& renderer, Rect rect, const char* text, int font = UI_10_FONT_ID,
              EpdFontFamily::Style style = EpdFontFamily::REGULAR, bool centered = false, bool black = true) {
  if (!text || !*text || rect.width <= 12 || rect.height <= 0) return 0;
  const int lineHeight = renderer.getLineHeight(font);
  const int capacity = rect.height / lineHeight;
  const char* p = text;
  int row = 0;
  while (*p && row < capacity) {
    char line[192] = {};
    size_t used = 0;
    while (*p && *p != '\n') {
      size_t bytes = 1;
      while (p[bytes] && (static_cast<unsigned char>(p[bytes]) & 0xC0) == 0x80) ++bytes;
      if (used + bytes >= sizeof(line)) break;
      memcpy(line + used, p, bytes);
      line[used + bytes] = '\0';
      if (renderer.getTextWidth(font, line, style) > rect.width - 8) {
        line[used] = '\0';
        break;
      }
      used += bytes;
      p += bytes;
    }
    if (!used && *p != '\n') break;
    if (*p == '\n') ++p;
    const int x = centered ? rect.x + (rect.width - renderer.getTextWidth(font, line, style)) / 2 : rect.x;
    renderer.drawText(font, x, rect.y + row * lineHeight, line, black, style);
    ++row;
  }
  if (*p && capacity > 0) {
    const int x = rect.x + rect.width - 6;
    const int y = rect.y + std::min(row, capacity) * lineHeight - 6;
    renderer.drawLine(x - 2, y, x, y + 2, black);
    renderer.drawLine(x, y + 2, x + 2, y, black);
  }
  return row * lineHeight;
}

int labelFont(const GfxRenderer& renderer, const char* text, int width) {
  return renderer.getTextWidth(UI_10_FONT_ID, text) <= width - 8 ? UI_10_FONT_ID : SMALL_FONT_ID;
}

int linesHeight(const GfxRenderer& renderer, const int fontId, const int lineCount) {
  return renderer.getLineHeight(fontId) * lineCount;
}

// Draws wrapped lines starting at `y` and returns the y after the last one.
int drawLines(const GfxRenderer& renderer, const std::vector<std::string>& lines, const int fontId, const int x, int y,
              const EpdFontFamily::Style style) {
  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& line : lines) {
    renderer.drawText(fontId, x, y, line.c_str(), true, style);
    y += lineHeight;
  }
  return y;
}
}  // namespace

void BaizeTheme::drawSurface(const GfxRenderer& renderer, Rect rect, bool selected) const {
  if (rect.width < 8 || rect.height < 8) return;
  const int radius = std::min(cardRadius, std::min(rect.width, rect.height) / 3);
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, selected ? 3 : 1, radius, true);
}

void BaizeTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const int badgeWidth = rect.height >= 70 ? 40 : 24;
  companion(renderer, rect.x + inset, rect.y + (rect.height - badgeWidth) / 2, badgeWidth);
  drawBatteryRight(renderer, Rect{rect.x + rect.width - 28, rect.y + 4, 16, 12},
                   SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);
  const int x = rect.x + inset + badgeWidth + 12;
  // Keep the first title line clear of the battery icon and its percentage at the top right.
  constexpr int batteryReserve = 48;
  const int available = rect.width - (x - rect.x) - inset - batteryReserve;
  int font = UI_12_FONT_ID;
  if (title && renderer.getTextWidth(font, title, EpdFontFamily::BOLD) > available - 8) font = UI_10_FONT_ID;
  const int titleLine = renderer.getLineHeight(font);
  const bool hasSubtitle = subtitle && *subtitle;
  const int subtitleLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int titleLines = hasSubtitle ? 1 : std::min(2, std::max(1, (rect.height - 8) / titleLine));
  const int blockHeight = titleLine * titleLines + (hasSubtitle ? subtitleLine + 2 : 0);
  const int blockTop = rect.y + std::max(2, (rect.height - blockHeight) / 2);
  textBlock(renderer, Rect{x, blockTop, available, titleLine * titleLines}, title, font, EpdFontFamily::BOLD);
  if (hasSubtitle) {
    textBlock(renderer, Rect{x, blockTop + titleLine + 2, available, subtitleLine}, subtitle, SMALL_FONT_ID);
  }
  renderer.drawLine(x, rect.y + rect.height - 2, rect.x + rect.width - inset, rect.y + rect.height - 2, true);
}

void BaizeTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                               const char* rightLabel) const {
  int rightWidth =
      rightLabel && *rightLabel ? std::min(rect.width / 2, renderer.getTextWidth(SMALL_FONT_ID, rightLabel) + 24) : 0;
  if (rightWidth) {
    const Rect chip{rect.x + rect.width - inset - rightWidth, rect.y + 2, rightWidth, rect.height - 4};
    drawSurface(renderer, chip);
    textBlock(renderer, Rect{chip.x + 8, chip.y + 3, chip.width - 16, chip.height - 6}, rightLabel, SMALL_FONT_ID,
              EpdFontFamily::REGULAR, true);
  }
  textBlock(renderer, Rect{rect.x + inset + 8, rect.y + 5, rect.width - inset * 2 - rightWidth - 16, rect.height - 6},
            label, UI_10_FONT_ID);
}

void BaizeTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                            bool selected) const {
  if (tabs.empty()) return;
  const int width = (rect.width - inset * 2) / static_cast<int>(tabs.size());
  for (size_t i = 0; i < tabs.size(); ++i) {
    const Rect tab{rect.x + inset + static_cast<int>(i) * width + 2, rect.y + 3, width - 4, rect.height - 6};
    const bool active = tabs[i].selected;
    if (active && selected) {
      renderer.fillRoundedRect(tab.x, tab.y, tab.width, tab.height, 12, Color::Black);
    } else if (active) {
      drawSurface(renderer, tab, true);
    }
    const int font = labelFont(renderer, tabs[i].label, tab.width - 8);
    textBlock(renderer,
              Rect{tab.x + 4, tab.y + (tab.height - renderer.getLineHeight(font)) / 2, tab.width - 8,
                   renderer.getLineHeight(font)},
              tabs[i].label, font, EpdFontFamily::REGULAR, true, !(active && selected));
  }
}

int BaizeTheme::getListPageItems(int height, bool subtitle) const {
  return std::max(
      1, height / (subtitle ? BaizeMetrics::values.listWithSubtitleRowHeight : BaizeMetrics::values.listRowHeight));
}

void BaizeTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                          const std::function<std::string(int)>& rowTitle,
                          const std::function<std::string(int)>& rowSubtitle, const std::function<UIIcon(int)>& rowIcon,
                          const std::function<std::string(int)>& rowValue, bool highlightValue,
                          const std::function<bool(int)>& rowDimmed) const {
  if (itemCount <= 0 || rect.height < 8) return;
  const int rowHeight =
      rowSubtitle ? BaizeMetrics::values.listWithSubtitleRowHeight : BaizeMetrics::values.listRowHeight;
  const int perPage = getListPageItems(rect.height, rowSubtitle != nullptr);
  const int start = std::max(0, selectedIndex) / perPage * perPage;
  const int pages = (itemCount + perPage - 1) / perPage;
  for (int i = start; i < std::min(itemCount, start + perPage); ++i) {
    const Rect card{rect.x + inset, rect.y + (i - start) * rowHeight, rect.width - inset * 2,
                    std::min(rowHeight - 6, rect.height - (i - start) * rowHeight)};
    const bool active = i == selectedIndex;
    if (active) drawSurface(renderer, card, true);
    int x = card.x + 12;
    if (rowIcon) {
      const auto* icon = iconBitmap(rowIcon(i), 24);
      if (icon) renderer.drawIcon(icon, x, card.y + 10, 24, 24);
      x += 32;
    }
    const std::string title = rowTitle(i);
    const std::string value = rowValue ? rowValue(i) : "";
    const int valueWidth =
        value.empty() ? 0 : std::min(card.width / 2, renderer.getTextWidth(UI_10_FONT_ID, value.c_str()) + 24);
    const int titleWidth = card.x + card.width - 12 - x - valueWidth;
    const auto style = active && !(rowDimmed && rowDimmed(i)) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int font = labelFont(renderer, title.c_str(), titleWidth);
    textBlock(renderer, Rect{x, card.y + 4, titleWidth, 46}, title.c_str(), font, style);
    if (rowSubtitle) {
      const std::string subtitle = rowSubtitle(i);
      textBlock(renderer, Rect{x, card.y + 50, card.x + card.width - 12 - x, card.height - 52}, subtitle.c_str(),
                SMALL_FONT_ID);
    }
    if (!value.empty()) {
      const Rect chip{card.x + card.width - valueWidth - 6, card.y + 5, valueWidth,
                      rowSubtitle ? 36 : card.height - 10};
      if (active && highlightValue) renderer.fillRoundedRect(chip.x, chip.y, chip.width, chip.height, 10, Color::Black);
      textBlock(renderer, Rect{chip.x + 8, chip.y + 2, chip.width - 12, chip.height - 4}, value.c_str(),
                labelFont(renderer, value.c_str(), chip.width - 12), EpdFontFamily::REGULAR, true,
                !(active && highlightValue));
    }
  }
  if (pages > 1) {
    const int x = rect.x + rect.width - 9;
    const int thumb = std::max(8, rect.height / pages);
    renderer.drawLine(x, rect.y, x, rect.y + rect.height - 1, true);
    renderer.fillRoundedRect(x - 2, rect.y + (rect.height - thumb) * (start / perPage) / (pages - 1), 5, thumb, 2,
                             Color::Black);
  }
}

void BaizeTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                 const char* btn4) const {
  const auto orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const int width = renderer.getScreenWidth();
  const int y = renderer.getScreenHeight() - BaizeMetrics::values.buttonHintsHeight + 2;
  const char* labels[] = {btn1, btn2, btn3, btn4};
#if defined(ONEPAGE_C61) || defined(ONEPAGE_S3)
  const int anchors[] = {20, 150, 250, 380};
#else
  const int anchors[] = {width == 528 ? 65 : 58, width == 528 ? 157 : 146, width == 528 ? 291 : 254,
                         width == 528 ? 383 : 342};
#endif
  HintBox boxes[4];
  layoutHintBoxes(renderer, SMALL_FONT_ID, labels, 4, anchors, 80, 4, width - 4, boxes);
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  for (int i = 0; i < 4; ++i) {
    if (!labels[i] || !*labels[i]) continue;
    const Rect chip{boxes[i].pos, y, boxes[i].length, 36};
    renderer.fillRoundedRect(chip.x, chip.y, chip.width, chip.height, 12, Color::White);
    drawSurface(renderer, chip);
    // Hint boxes are already sized from the label plus padding, so draw the
    // original text on one line. Wrapping it here added a second inset and
    // clipped the last character of labels such as "查代码".
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
    renderer.drawText(SMALL_FONT_ID, chip.x + (chip.width - textWidth) / 2, chip.y + (chip.height - lineHeight) / 2,
                      labels[i]);
  }
  renderer.setOrientation(orientation);
}

void BaizeTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int h = renderer.getScreenHeight();
  const int x = renderer.getScreenWidth() - BaizeMetrics::values.sideButtonHintsWidth;
  const char* labels[] = {topBtn, bottomBtn};
  for (int i = 0; i < 2; ++i) {
    if (!labels[i] || !*labels[i]) continue;
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
    // The chip must hold the text's full advance plus a visible margin; a
    // 16 px budget left labels such as "前一天" touching the rounded border.
    const int length = std::min(h / 2 - 16, std::max(78, textWidth + 24));
    const int y = h / 2 + (i == 0 ? -length - 4 : 4);
    const int sideX = gpio.deviceIsX3() && i == 0 ? 0 : x;
    drawSurface(renderer, Rect{sideX, y, 29, length});
    renderer.drawTextRotated90CW(SMALL_FONT_ID, sideX + 2, y + (length + textWidth) / 2, labels[i]);
  }
}

void BaizeTheme::drawHomeIconGrid(GfxRenderer& renderer, Rect rect, int count, int selected, int columns,
                                  const std::function<std::string(int)>& label,
                                  const std::function<UIIcon(int)>& icon) const {
  if (count <= 0 || columns <= 0) return;
  const int width = (rect.width - 2 * inset) / columns;
  // Two label lines: four Chinese characters do not fit a four-column tile
  // at the 10 pt UI font, and a truncated button name is not acceptable.
  const int height = BaizeMetrics::values.menuRowHeight + 32;
  for (int i = 0; i < count; ++i) {
    const Rect card{rect.x + inset + (i % columns) * width + 2,
                    rect.y + (i / columns) * (height + BaizeMetrics::values.menuSpacing), width - 4, height - 4};
    drawSurface(renderer, card, i == selected);
    const auto* bitmap = icon ? iconBitmap(icon(i), 32) : nullptr;
    if (bitmap) renderer.drawIcon(bitmap, card.x + (card.width - 32) / 2, card.y + 8, 32, 32);
    const std::string text = label(i);
    textBlock(renderer, Rect{card.x + 4, card.y + 42, card.width - 8, card.height - 44}, text.c_str(),
              labelFont(renderer, text.c_str(), card.width - 8), EpdFontFamily::REGULAR, true);
  }
}

void BaizeTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int count, int selected,
                                const std::function<std::string(int)>& label,
                                const std::function<UIIcon(int)>& icon) const {
  for (int i = 0; i < count; ++i) {
    const Rect card{rect.x + inset, rect.y + i * (BaizeMetrics::values.menuRowHeight + 8), rect.width - 2 * inset,
                    BaizeMetrics::values.menuRowHeight};
    if (card.y + card.height > rect.y + rect.height) break;
    drawSurface(renderer, card, i == selected);
    const auto* bitmap = icon ? iconBitmap(icon(i), 32) : nullptr;
    if (bitmap) renderer.drawIcon(bitmap, card.x + 12, card.y + 16, 32, 32);
    const std::string text = label(i);
    const int offset = bitmap ? 56 : 16;
    textBlock(renderer, Rect{card.x + offset, card.y + 8, card.width - offset - 12, card.height - 16}, text.c_str(),
              UI_10_FONT_ID, i == selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}

void BaizeTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& books, int index,
                                     bool selected, bool& rendered, bool& stored, bool& restored,
                                     std::function<bool()> store) const {
  LyraTheme::drawRecentBookCover(renderer, rect, books, index, false, rendered, stored, restored, store);
  drawSurface(renderer, Rect{rect.x + inset, rect.y, rect.width - inset * 2, rect.height}, selected);
  if (!books.empty()) {
    const auto& book = books[index >= 0 && index < static_cast<int>(books.size()) ? index : 0];
    {
      // Reserve the card's bottom strip; cover caching and book metadata stay unchanged.
      const int x = rect.x + rect.width / 2;
      const int width = rect.width / 2 - inset - 12;
      renderer.fillRect(x, rect.y + rect.height - 46, width, 28, false);
      char progress[12];
      snprintf(progress, sizeof(progress), "%d%%", book.progressPercent);
      textBlock(renderer, Rect{x, rect.y + rect.height - 46, width, 24}, progress, SMALL_FONT_ID);
      drawProgressBar(renderer, Rect{x, rect.y + rect.height - 16, width, 8},
                      std::clamp<int>(book.progressPercent, 0, 100), 100);
    }
  }
}

void BaizeTheme::drawChoiceCards(GfxRenderer& renderer, Rect rect, int count, int selected,
                                 const ChoiceCard* cards) const {
  if (!cards || count <= 0) return;
  const int height = (rect.height - (count - 1) * 16) / count;
  if (height < 20) return;
  for (int i = 0; i < count; ++i) {
    const Rect card{rect.x + inset, rect.y + i * (height + 16), rect.width - inset * 2, height};
    drawSurface(renderer, card, i == selected);
    int y = card.y + 12;
    if (cards[i].icon && height >= 150) {
      renderer.drawIcon(cards[i].icon, card.x + (card.width - 64) / 2, y, 64, 64);
      y += 76;
    }
    y += textBlock(renderer, Rect{card.x + 16, y, card.width - 32, std::min(64, card.y + height - y - 12)},
                   cards[i].label, UI_12_FONT_ID, EpdFontFamily::BOLD, true);
    textBlock(renderer, Rect{card.x + 16, y + 8, card.width - 32, card.y + height - y - 20}, cards[i].hint,
              SMALL_FONT_ID, EpdFontFamily::REGULAR, true);
  }
}

void BaizeTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const {
  if (rect.width < 6 || rect.height < 6) return;
  renderer.fillRoundedRect(rect.x, rect.y, rect.width, rect.height, rect.height / 2, Color::White);
  renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, rect.height / 2, true);
  const int width =
      total ? static_cast<int>((rect.width - 4) * static_cast<uint64_t>(std::min(current, total)) / total) : 0;
  if (width > 0)
    renderer.fillRoundedRect(rect.x + 2, rect.y + 2, width, rect.height - 4, std::min(width, rect.height - 4) / 2,
                             Color::Black);
}

Rect BaizeTheme::drawPopup(const GfxRenderer& renderer, const char* message, bool progress, bool display) const {
  const int width = renderer.getScreenWidth() - 48;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int height = 32 + lineHeight * 3 + (progress ? 52 : 0);
  const Rect card{24, (renderer.getScreenHeight() - height) / 3, width, height};
  renderer.fillRoundedRect(card.x, card.y, card.width, card.height, cardRadius, Color::White);
  drawSurface(renderer, card, true);
  companion(renderer, card.x + 12, card.y + 12, 32);
  textBlock(renderer, Rect{card.x + 56, card.y + 16, width - 72, lineHeight * 3}, message, UI_12_FONT_ID,
            EpdFontFamily::BOLD);
  if (display) renderer.displayBuffer();
  return card;
}

void BaizeTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& rect, int percent, bool label,
                                   bool display) const {
  percent = std::clamp(percent, 0, 100);
  drawProgressBar(renderer, Rect{rect.x + 16, rect.y + rect.height - 48, rect.width - 32, 12}, percent, 100);
  renderer.fillRect(rect.x + 16, rect.y + rect.height - 30, rect.width - 32, 24, false);
  if (label) {
    char text[8];
    snprintf(text, sizeof(text), "%d%%", percent);
    textBlock(renderer, Rect{rect.x + 16, rect.y + rect.height - 30, rect.width - 32, 24}, text, SMALL_FONT_ID,
              EpdFontFamily::REGULAR, true);
  }
  if (display) renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

bool BaizeTheme::drawSystemMessage(const GfxRenderer& renderer, Rect rect, const char* title, const char* body) const {
  const int width = rect.width - 48;
  const int height = std::min(320, rect.height - 32);
  const Rect card{rect.x + 24, rect.y + (rect.height - height) / 2, width, height};
  if (height < 80) return false;
  drawSurface(renderer, card);
  const int badge = height >= 240 ? 64 : 32;
  companion(renderer, card.x + (width - badge) / 2, card.y + 12, badge);
  const int top = card.y + 20 + badge * 128 / SPRITE_W;
  const int bottom = card.y + height - 16;
  if (!body || !*body) {
    // Full-screen notices pass the whole message as the title; give it the
    // remaining card height instead of clipping it to two lines.
    textBlock(renderer, Rect{card.x + 16, top, width - 32, bottom - top}, title, UI_12_FONT_ID, EpdFontFamily::BOLD,
              true);
    return true;
  }
  const int titleHeight =
      textBlock(renderer, Rect{card.x + 16, top, width - 32, 64}, title, UI_12_FONT_ID, EpdFontFamily::BOLD, true);
  textBlock(renderer, Rect{card.x + 16, top + titleHeight + 12, width - 32, bottom - top - titleHeight - 12}, body,
            UI_10_FONT_ID, EpdFontFamily::REGULAR, true);
  return true;
}

int BaizeTheme::drawReaderDecoration(const GfxRenderer& renderer, int x, int y) const {
  // A 16 px mascot turns into an unreadable smudge next to 8 pt status text;
  // the reader lane gets a quiet rounded accent instead.
  const int height = std::max(8, renderer.getLineHeight(SMALL_FONT_ID) - 4);
  renderer.fillRoundedRect(x, y + 2, 4, height, 2, Color::Black);
  return 14;
}

void BaizeTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  // Callers reserve as little as 20 px, which is shorter than the 8 pt line
  // height. Truncate a single line instead of dropping the hint entirely.
  const auto text = renderer.truncatedText(SMALL_FONT_ID, label, rect.width - inset * 2, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, rect.y, text.c_str());
}

void BaizeTheme::drawTextField(const GfxRenderer& renderer, Rect rect, int textWidth, bool cursorMode,
                               int contentStartX, int contentWidth) const {
  // Match KeyboardEntryActivity's input baseline; rect itself is the heading region.
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int bottom = rect.y + rect.height + lineHeight + BaizeMetrics::values.verticalSpacing;
  const int width = contentWidth > 0 ? contentWidth : std::min(rect.width - 16, textWidth + 24);
  const int x = contentWidth > 0 ? rect.x + contentStartX : rect.x + (rect.width - width) / 2;
  // Enclose the underline without covering the hint line drawn 4 px below it.
  drawSurface(renderer, Rect{x - 4, bottom - lineHeight - 8, width + 8, lineHeight + 10}, cursorMode);
}

bool BaizeTheme::drawHomeGreeting(const GfxRenderer& renderer, Rect rect, const bool /*hasRecentBook*/) const {
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - BaizeMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, BaizeMetrics::values.batteryWidth, BaizeMetrics::values.batteryHeight},
                   showBatteryPercentage);

  const int spriteX = rect.x + rect.width - SPRITE_W - spriteRightGap;
  companion(renderer, spriteX, rect.y + spriteTop, SPRITE_W);

  const int textX = rect.x + textLeftPadding;
  const int textW = spriteX - textSpriteGap - textX;
  const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, tr(STR_BAIZE_HOME_TITLE), textW, 2, EpdFontFamily::BOLD);
  const auto tagLines = renderer.wrappedText(SMALL_FONT_ID, tr(STR_BAIZE_TAGLINE), textW, 2);

  const int titleH = linesHeight(renderer, UI_12_FONT_ID, static_cast<int>(titleLines.size()));
  const int tagH = linesHeight(renderer, SMALL_FONT_ID, static_cast<int>(tagLines.size()));
  const int totalH = titleH + 12 + tagH;
  int y = rect.y + (rect.height - totalH) / 2;

  y = drawLines(renderer, titleLines, UI_12_FONT_ID, textX, y, EpdFontFamily::BOLD);
  y += 12;
  drawLines(renderer, tagLines, SMALL_FONT_ID, textX, y, EpdFontFamily::REGULAR);
  return true;
}
