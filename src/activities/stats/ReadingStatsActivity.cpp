#include "ReadingStatsActivity.h"

#include <ChatHistoryFormat.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ReadingAggregate.h>
#include <ReadingSessionTracker.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Local day index (days since 1970-01-01) for an epoch-second timestamp, using
// the same offset encoding as every other day-bucketed file on the card.
int32_t localDayIndex(const int64_t epochSec) {
  const int64_t localSec = ChatLog::ChatHistoryFormat::localEpochSeconds(epochSec * 1000, SETTINGS.clockUtcOffsetQ);
  return static_cast<int32_t>(localSec >= 0 ? localSec / 86400 : (localSec - 86399) / 86400);
}

// "42 min" — rounded up so a short session never reads as "0 min".
void formatMinutes(char* out, const size_t cap, const uint32_t sec) {
  snprintf(out, cap, "%u %s", static_cast<unsigned>((sec + 59) / 60), tr(STR_MINUTES_SHORT));
}

}  // namespace

void ReadingStatsActivity::onEnter() {
  Activity::onEnter();
  loadStats();
  requestUpdate();
}

void ReadingStatsActivity::loadStats() {
  const int64_t nowSec = static_cast<int64_t>(time(nullptr));
  clockValid_ = nowSec >= ReadingHistory::MIN_SANE_EPOCH_SEC;
  if (!clockValid_) {
    LOG_INF("STATS", "clock unsynced (%lld), stats unavailable", static_cast<long long>(nowSec));
    return;
  }

  today_ = localDayIndex(nowSec);
  first_ = ReadingHistory::heatmapFirstDayIndex(today_);
  daysWithData_ = ReadingHistory::loadDayStats(first_, ReadingHistory::HEATMAP_DAYS, days_, true);
  LOG_DBG("STATS", "window %d..%d, %d day(s) with data", static_cast<int>(first_), static_cast<int>(today_),
          daysWithData_);
}

uint32_t ReadingStatsActivity::sumWindow(const int32_t fromDayIndex, const int32_t toDayIndex) const {
  uint32_t total = 0;
  for (int i = 0; i < ReadingHistory::HEATMAP_DAYS; ++i) {
    const auto& day = days_[i];
    if (day.dayIndex < fromDayIndex || day.dayIndex > toDayIndex || !day.present) continue;
    total += day.totalSec;
  }
  return total;
}

int ReadingStatsActivity::countActiveDays(const int32_t fromDayIndex, const int32_t toDayIndex) const {
  int count = 0;
  for (int i = 0; i < ReadingHistory::HEATMAP_DAYS; ++i) {
    const auto& day = days_[i];
    if (day.dayIndex < fromDayIndex || day.dayIndex > toDayIndex) continue;
    if (day.present && day.totalSec > 0) ++count;
  }
  return count;
}

void ReadingStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::STATS);
  }
}

void ReadingStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  char subtitle[24] = {};
  if (clockValid_) {
    char firstDate[16];
    char lastDate[16];
    if (ReadingHistory::formatDayDate(firstDate, sizeof(firstDate), first_) &&
        ReadingHistory::formatDayDate(lastDate, sizeof(lastDate), first_ + ReadingHistory::HEATMAP_DAYS - 1)) {
      // "MM-DD ~ MM-DD" (drop the year from both ends; the header has no room).
      snprintf(subtitle, sizeof(subtitle), "%s ~ %s", firstDate + 5, lastDate + 5);
    }
  }

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_STATS),
                 clockValid_ ? subtitle : nullptr);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (!clockValid_) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 40, tr(STR_READING_CLOCK_NEEDED));
  } else {
    drawSummary(contentTop, metrics.contentSidePadding);
    // Summary block: three UI_10 lines plus the gap below them.
    const int summaryH = 3 * (renderer.getLineHeight(UI_10_FONT_ID) + 4) + 12;
    drawHeatmap(contentTop + summaryH, contentBottom, pageWidth);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void ReadingStatsActivity::drawSummary(const int contentTop, const int sidePad) const {
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int valueX = sidePad + 150;
  int y = contentTop + 2;

  const int todaySlot = today_ - first_;
  const ReadingHistory::DayStat* today =
      (todaySlot >= 0 && todaySlot < ReadingHistory::HEATMAP_DAYS) ? &days_[todaySlot] : nullptr;

  char value[48];
  if (today != nullptr && today->present && today->totalSec > 0) {
    char minutes[24];
    formatMinutes(minutes, sizeof(minutes), today->totalSec);
    snprintf(value, sizeof(value), "%s  %u %s", minutes, static_cast<unsigned>(today->pagesAdvanced),
             tr(STR_PAGES_SHORT));
    renderer.drawText(UI_10_FONT_ID, sidePad, y, tr(STR_TODAY));
    renderer.drawText(UI_10_FONT_ID, valueX, y, value);
  } else {
    renderer.drawText(UI_10_FONT_ID, sidePad, y, tr(STR_TODAY));
    renderer.drawText(UI_10_FONT_ID, valueX, y, tr(STR_NO_READING_DATA));
  }
  y += lineH + 4;

  char minutes[24];
  formatMinutes(minutes, sizeof(minutes), sumWindow(today_ - 6, today_));
  renderer.drawText(UI_10_FONT_ID, sidePad, y, tr(STR_READING_LAST_7_DAYS));
  renderer.drawText(UI_10_FONT_ID, valueX, y, minutes);
  y += lineH + 4;

  snprintf(value, sizeof(value), "%d", countActiveDays(first_, today_));
  renderer.drawText(UI_10_FONT_ID, sidePad, y, tr(STR_READING_ACTIVE_DAYS));
  renderer.drawText(UI_10_FONT_ID, valueX, y, value);
}

void ReadingStatsActivity::drawHeatmap(const int contentTop, const int contentBottom, const int pageWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int sidePad = metrics.contentSidePadding;
  constexpr int weekdayLabelW = 34;
  constexpr int gap = 4;
  constexpr int legendH = 20;
  const int rowLabelH = renderer.getLineHeight(SMALL_FONT_ID) + 2;

  const int gridTop = contentTop + rowLabelH;
  const int availW = pageWidth - sidePad * 2 - weekdayLabelW;
  const int availH = contentBottom - gridTop - legendH - 8;
  if (availW <= 0 || availH <= 0) return;

  int cell = std::min((availW - (ReadingHistory::HEATMAP_WEEKS - 1) * gap) / ReadingHistory::HEATMAP_WEEKS,
                      (availH - 6 * gap) / 7);
  if (cell < 6) cell = 6;

  const int gridW = ReadingHistory::HEATMAP_WEEKS * cell + (ReadingHistory::HEATMAP_WEEKS - 1) * gap;
  const int gridH = 7 * cell + 6 * gap;
  const int gridX = sidePad + weekdayLabelW + std::max(0, (availW - gridW) / 2);
  const int gridY = gridTop + std::max(0, (availH - gridH) / 2);

  const char* const weekdayNames[7] = {tr(STR_WD_MON), tr(STR_WD_TUE), tr(STR_WD_WED), tr(STR_WD_THU),
                                       tr(STR_WD_FRI), tr(STR_WD_SAT), tr(STR_WD_SUN)};

  // Month stamps above the column whose Monday starts a new month.
  int lastMonth = -1;
  for (int col = 0; col < ReadingHistory::HEATMAP_WEEKS; ++col) {
    const int32_t dayIndex = first_ + col * 7;
    int year = 0;
    int month = 0;
    int dom = 0;
    ChatLog::ChatHistoryFormat::civilFromDays(dayIndex, year, month, dom);
    if (month == lastMonth) continue;
    lastMonth = month;
    char label[8];
    snprintf(label, sizeof(label), "%d", month);
    renderer.drawText(SMALL_FONT_ID, gridX + col * (cell + gap), gridTop - rowLabelH, label);
  }

  for (int row = 0; row < 7; ++row) {
    const int y = gridY + row * (cell + gap);
    const int labelW = renderer.getTextWidth(SMALL_FONT_ID, weekdayNames[row]);
    renderer.drawText(SMALL_FONT_ID, gridX - 6 - labelW, y + (cell - renderer.getLineHeight(SMALL_FONT_ID)) / 2,
                      weekdayNames[row]);

    for (int col = 0; col < ReadingHistory::HEATMAP_WEEKS; ++col) {
      const int slot = col * 7 + row;
      if (slot < 0 || slot >= ReadingHistory::HEATMAP_DAYS) continue;
      const auto& day = days_[slot];
      // Future days of the current week have no data and no cell.
      if (day.dayIndex > today_) continue;

      const int x = gridX + col * (cell + gap);
      switch (ReadingHistory::heatLevel(day.present ? day.totalSec : 0)) {
        case 0:
          renderer.drawRect(x, y, cell, cell);
          break;
        case 1:
          renderer.fillRectDither(x, y, cell, cell, Color::LightGray);
          break;
        case 2:
          renderer.fillRectDither(x, y, cell, cell, Color::DarkGray);
          break;
        default:
          renderer.fillRect(x, y, cell, cell);
          break;
      }
    }
  }

  // Legend: 0 / <15 / 15–40 / >40 minutes, using the same four patches.
  const int legendY = gridY + gridH + 10;
  const int box = 10;
  int x = gridX;
  const char* const marks[4] = {"0", "<15", "15-40", ">40"};
  for (int i = 0; i < 4; ++i) {
    const int y = legendY + (renderer.getLineHeight(SMALL_FONT_ID) - box) / 2;
    switch (i) {
      case 0:
        renderer.drawRect(x, y, box, box);
        break;
      case 1:
        renderer.fillRectDither(x, y, box, box, Color::LightGray);
        break;
      case 2:
        renderer.fillRectDither(x, y, box, box, Color::DarkGray);
        break;
      default:
        renderer.fillRect(x, y, box, box);
        break;
    }
    renderer.drawText(SMALL_FONT_ID, x + box + 4, legendY, marks[i]);
    x += box + 4 + renderer.getTextWidth(SMALL_FONT_ID, marks[i]) + 14;
  }
  renderer.drawText(SMALL_FONT_ID, x, legendY, tr(STR_MINUTES_SHORT));
}
