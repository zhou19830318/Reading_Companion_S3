#pragma once

#include <ChatHistoryFormat.h>
#include <ReadingSessionFormat.h>

#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"

// Time helpers for the RP-01/02 editors. Kept out of the plan lib so the
// format/store layers stay testable without a clock or settings singleton.
namespace PlanTime {

inline constexpr int64_t MIN_SANE_EPOCH_SEC = 1600000000LL;

inline int64_t nowSec() { return static_cast<int64_t>(time(nullptr)); }

// Same floor as the reading history: an unsynced ESP32 reports 1970, and a
// plan stamped with that date would be wrong forever.
inline bool clockValid() { return nowSec() >= MIN_SANE_EPOCH_SEC; }

inline bool nowIso(char* out, size_t cap) {
  return ReadingHistory::formatIsoTimestamp(out, cap, nowSec(), SETTINGS.clockUtcOffsetQ);
}

// Today's local day index (days since 1970-01-01) and weekday for the RP-03
// advice (0 = Sunday .. 6 = Saturday, the FreeSlot mask order). False when the
// clock is unsynced, in which case the caller can still show minute advice but
// must not propose time segments for a guessed weekday.
inline bool todayDayIndex(int32_t& dayIndexOut, int& weekdayOut) {
  const int64_t now = nowSec();
  if (now < MIN_SANE_EPOCH_SEC) return false;
  const int64_t localSec = ChatLog::ChatHistoryFormat::localEpochSeconds(now * 1000, SETTINGS.clockUtcOffsetQ);
  const int64_t days = (localSec >= 0 ? localSec : localSec - 86399) / 86400;
  dayIndexOut = static_cast<int32_t>(days);
  // 1970-01-01 was a Thursday (4 with Sunday = 0), so shift by 4 and normalize.
  weekdayOut = static_cast<int>(((days + 4) % 7 + 7) % 7);
  return true;
}

// Day index of a "YYYY-MM-DD" string; false for empty/invalid dates.
inline bool dayIndexFromDate(const char* date, int32_t& dayIndexOut) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (date == nullptr || !ChatLog::ChatHistoryFormat::parseDayDate(date, year, month, day)) return false;
  dayIndexOut = static_cast<int32_t>(ChatLog::ChatHistoryFormat::daysFromCivil(year, month, day));
  return true;
}

// "YYYY-MM-DD" for today plus `daysAhead` in the user's local time.
inline bool dateInDays(int daysAhead, char* out, size_t cap) {
  const int64_t now = nowSec();
  if (now < MIN_SANE_EPOCH_SEC || out == nullptr || cap < 11) return false;
  const int64_t localSec = ChatLog::ChatHistoryFormat::localEpochSeconds(now * 1000, SETTINGS.clockUtcOffsetQ);
  const int64_t days = (localSec >= 0 ? localSec : localSec - 86399) / 86400 + daysAhead;
  int year = 0;
  int month = 0;
  int day = 0;
  ChatLog::ChatHistoryFormat::civilFromDays(days, year, month, day);
  const int n = snprintf(out, cap, "%04d-%02d-%02d", year, month, day);
  return n > 0 && static_cast<size_t>(n) < cap;
}

}  // namespace PlanTime
