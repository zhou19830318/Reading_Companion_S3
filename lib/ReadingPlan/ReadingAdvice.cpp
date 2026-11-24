#include "ReadingAdvice.h"

#include <algorithm>

namespace ReadingPlan {

namespace {

uint16_t clampMinutes(const uint32_t value) {
  if (value < ADVICE_MINUTES_MIN) return ADVICE_MINUTES_MIN;
  if (value > ADVICE_MINUTES_MAX) return ADVICE_MINUTES_MAX;
  return static_cast<uint16_t>(value);
}

}  // namespace

Advice buildAdvice(const Plan& plan, const Slots& slots, const PaceWindow& window, const int todayWeekday,
                   const int32_t targetDayIndex, const int32_t todayDayIndex) {
  Advice advice;

  // Weekly goal -> per-day goal. Every positive value rounds up: asking for
  // 180 min/week must not silently become 25 min/day (175/week).
  advice.dailyMinutes = clampMinutes((static_cast<uint32_t>(plan.weeklyMinutes) + 6) / 7);

  if (window.totalSec >= 60 && window.pagesAdvanced > 0) {
    const uint64_t pagesPerMinX100 =
        (static_cast<uint64_t>(window.pagesAdvanced) * 6000u) / static_cast<uint64_t>(window.totalSec);
    if (pagesPerMinX100 > 0) {
      advice.hasPace = true;
      advice.pacePagesPerMinX100 = static_cast<uint16_t>(std::min<uint64_t>(pagesPerMinX100, UINT16_MAX));
    }
  }

  if (plan.weeklyPages > 0) {
    advice.pagesPerDay = static_cast<uint16_t>((static_cast<uint32_t>(plan.weeklyPages) + 6) / 7);
    if (advice.hasPace && advice.pacePagesPerMinX100 > 0) {
      // pages_per_day / (pages/min) expressed in the ×100 fixed point.
      const uint32_t minutes = (static_cast<uint32_t>(advice.pagesPerDay) * 100u + advice.pacePagesPerMinX100 - 1u) /
                               advice.pacePagesPerMinX100;
      advice.dailyMinutes = clampMinutes(minutes);
    }
  }

  if (targetDayIndex > todayDayIndex) {
    const int64_t left = static_cast<int64_t>(targetDayIndex) - todayDayIndex;
    advice.daysLeft = static_cast<int32_t>(std::min<int64_t>(left, INT32_MAX));
  }

  if (todayWeekday < 0 || todayWeekday > 6) return advice;

  // Split the daily minutes over today's slots in time order. A slot shorter
  // than the remaining minute budget simply contributes what fits; anything
  // left over stays unscheduled (the UI still reports the total).
  uint16_t remaining = advice.dailyMinutes;
  for (int i = 0; i < slots.count && advice.segmentCount < MAX_ADVICE_SEGMENTS && remaining > 0; ++i) {
    const FreeSlot& slot = slots.items[i];
    if (!daySelected(slot.daysMask, todayWeekday)) continue;
    if (slot.endMin <= slot.startMin) continue;

    uint16_t cursor = slot.startMin;
    while (cursor < slot.endMin && advice.segmentCount < MAX_ADVICE_SEGMENTS && remaining > 0) {
      const uint16_t capacity = static_cast<uint16_t>(slot.endMin - cursor);
      uint16_t take = std::min(capacity, remaining);
      take = std::min(take, ADVICE_SEGMENT_MINUTES);
      if (take < ADVICE_SEGMENT_MIN) break;  // no 3-minute "blocks"
      advice.segments[advice.segmentCount].startMin = cursor;
      advice.segments[advice.segmentCount].endMin = static_cast<uint16_t>(cursor + take);
      ++advice.segmentCount;
      advice.scheduledMinutes = static_cast<uint16_t>(advice.scheduledMinutes + take);
      cursor = static_cast<uint16_t>(cursor + take);
      remaining = static_cast<uint16_t>(remaining - take);
    }
  }

  return advice;
}

}  // namespace ReadingPlan
