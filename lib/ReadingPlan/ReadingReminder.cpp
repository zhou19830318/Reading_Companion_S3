#include "ReadingReminder.h"

namespace ReadingPlan {

ReminderStatus buildReminder(const bool enabled, const bool clockValid, const bool hasActivePlan,
                             const bool skippedToday, const uint32_t todaySec, const uint16_t dailyMinutes) {
  ReminderStatus status;
  if (!enabled || !clockValid || !hasActivePlan || skippedToday || dailyMinutes == 0) return status;

  status.totalMinutes = dailyMinutes;
  const uint32_t targetSec = static_cast<uint32_t>(dailyMinutes) * 60u;
  if (todaySec >= targetSec) {
    status.kind = ReminderStatus::Kind::Done;
    return status;
  }

  const uint32_t missing = targetSec - todaySec;
  status.remainingMinutes = static_cast<uint16_t>((missing + 59u) / 60u);
  status.kind = ReminderStatus::Kind::Remaining;
  return status;
}

}  // namespace ReadingPlan
