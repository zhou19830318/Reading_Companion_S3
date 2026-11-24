#pragma once

#include <ReadingAggregateFormat.h>

#include <cstdint>

#include "activities/Activity.h"

// 阅读统计 (RH-03): a static, full-refresh page showing the last 12 weeks of
// reading time as a GitHub-contribution style heatmap, plus today / last-7-day
// / active-day totals.
//
// Data comes from the RH-02 aggregate files; days recorded before RH-02
// shipped fall back to their RH-01 history JSONL (read-only, see
// ReadingHistory::loadDayStats). Nothing here writes to the SD card, and the
// page never runs during reading — it is a browsing surface reached from Home.
class ReadingStatsActivity final : public Activity {
 public:
  ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingStats", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  bool clockValid_ = false;
  int32_t today_ = 0;
  int32_t first_ = 0;  // first day of the 12-week window
  int daysWithData_ = 0;
  ReadingHistory::DayStat days_[ReadingHistory::HEATMAP_DAYS];

  void loadStats();
  uint32_t sumWindow(const int32_t fromDayIndex, const int32_t toDayIndex) const;
  int countActiveDays(const int32_t fromDayIndex, const int32_t toDayIndex) const;
  void drawSummary(const int contentTop, const int sidePad) const;
  void drawHeatmap(const int contentTop, const int contentBottom, const int pageWidth) const;
};
