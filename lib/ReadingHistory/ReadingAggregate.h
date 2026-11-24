#pragma once

#include <ReadingAggregateFormat.h>

#include <cstdint>

namespace ReadingHistory {

// RH-02: rebuild one day's aggregate from its RH-01 history JSONL and upsert
// the record into /.crosspoint/aggregate/<year>.json.
//
// `dayName` may be "YYYY-MM-DD" or "YYYY-MM-DD.jsonl" (the history file name).
// The year file is rewritten through a .tmp + rename, so a torn write can only
// lose the aggregate cache, never leave a half-valid document. Returns false
// when the history file is missing/empty or the SD write fails; the history
// record itself is never touched.
bool updateDayAggregate(const char* dayName);

// Read day totals for [firstDayIndex, firstDayIndex + count) into `out`.
// out[i].dayIndex is always filled; present is false for days with no data.
// With fallbackToHistory, a day that has no aggregate entry is computed from
// its history file instead (RH-01 days recorded before RH-02 shipped). The
// fallback only reads — it does not rewrite the year file, so opening the
// stats view cannot trigger a burst of SD writes.
// Returns the number of days that have data.
int loadDayStats(int32_t firstDayIndex, int count, DayStat* out, bool fallbackToHistory = true);

}  // namespace ReadingHistory
