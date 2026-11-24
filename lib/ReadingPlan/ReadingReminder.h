#pragma once

#include <cstdint>

// AI-08.5 gentle reminder decision. Pure integer logic so the sleep screen can
// stay a thin renderer and the rules are testable on the host:
//  - nothing at all unless the user opted in, the clock is sane, a plan is
//    active, and today was not skipped;
//  - "done" once today's reading reaches the plan's daily minutes;
//  - otherwise the rounded-up minutes still missing (a 20-second shortfall is
//    one minute, never zero).
// There is no notification channel here by design: the caller only draws a
// line on the sleep screen, never during reading.
namespace ReadingPlan {

struct ReminderStatus {
  enum class Kind : uint8_t { Hidden, Done, Remaining };

  Kind kind = Kind::Hidden;
  uint16_t totalMinutes = 0;
  uint16_t remainingMinutes = 0;
};

ReminderStatus buildReminder(bool enabled, bool clockValid, bool hasActivePlan, bool skippedToday, uint32_t todaySec,
                             uint16_t dailyMinutes);

}  // namespace ReadingPlan
