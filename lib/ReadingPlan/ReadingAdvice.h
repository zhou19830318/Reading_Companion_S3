#pragma once

#include <cstdint>

#include "ReadingPlanFormat.h"

// RP-03 (v4.0 §5.2): local, offline reading advice. Pure integer math over the
// plan, the user's free-time slots, and the last two weeks of reading history —
// no SD, no clock, no network, so the same code runs in the host tests and on
// the device.
//
// The v4.0 formula needs remaining_pages, which the firmware does not store
// (page counts only exist after a section is laid out, and not every section
// is). The local degradation therefore uses the data the device actually has:
//
//   daily_minutes = ceil(weekly_minutes / 7)
//   pages_per_day = ceil(weekly_pages / 7)
//   if pace is known and pages_per_day > 0:
//       daily_minutes = ceil(pages_per_day / pace)
//
// `pace` is derived from the reading aggregate (pages advanced / seconds read)
// over a caller-chosen window. Without either a pages goal or history, the
// weekly minute target stands on its own — which is exactly the offline case
// the acceptance criterion asks for ("create a plan → get a daily suggestion
// with no network").
//
// The daily minutes are then split into 1..3 concrete segments inside today's
// free slots, at most 30 minutes each. Segment count is capped so the advice
// stays a nudge, not a schedule.
namespace ReadingPlan {

inline constexpr int MAX_ADVICE_SEGMENTS = 3;
inline constexpr uint16_t ADVICE_MINUTES_MIN = 5;
inline constexpr uint16_t ADVICE_MINUTES_MAX = 240;
inline constexpr uint16_t ADVICE_SEGMENT_MINUTES = 30;  // longest single block
inline constexpr uint16_t ADVICE_SEGMENT_MIN = 10;      // shorter tail is dropped

struct PaceWindow {
  uint32_t totalSec = 0;
  uint32_t pagesAdvanced = 0;
  uint8_t daysWithData = 0;
};

struct AdviceSegment {
  uint16_t startMin = 0;  // minutes since local midnight
  uint16_t endMin = 0;
};

struct Advice {
  uint16_t dailyMinutes = 0;
  uint16_t pagesPerDay = 0;          // 0 when the plan tracks minutes only
  uint16_t pacePagesPerMinX100 = 0;  // pages/min × 100; 0 when unknown
  bool hasPace = false;
  int32_t daysLeft = 0;  // 0 when no (or already past) deadline
  uint8_t segmentCount = 0;
  AdviceSegment segments[MAX_ADVICE_SEGMENTS] = {};
  uint16_t scheduledMinutes = 0;  // minutes covered by segments
};

// `todayWeekday` is 0 = Sunday .. 6 = Saturday (FreeSlot.daysMask). Pass -1
// when the clock is unset: the minute advice still computes, but no segments
// are proposed because no weekday can be trusted.
Advice buildAdvice(const Plan& plan, const Slots& slots, const PaceWindow& window, int todayWeekday,
                   int32_t targetDayIndex, int32_t todayDayIndex);

}  // namespace ReadingPlan
