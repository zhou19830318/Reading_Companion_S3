#pragma once

#include <ChatHistoryFormat.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// RH-02 daily aggregate: one year file, one day per line inside a JSON array.
//
//   /.crosspoint/aggregate/2026.json
//   {"schema_version":1,"year":2026,"days":[
//   {"date":"2026-11-22","total_sec":1840,"sessions":3,"pages_advanced":27,"books":[...]},
//   {"date":"2026-11-23",...}
//   ]}
//
// The document is valid JSON, but it is laid out one day per line so the writer
// can stream-merge (rewrite one line, copy the rest) without a DOM: a full year
// is at most 366 lines, and the device never needs the whole file in RAM.
//
// Book distribution is capped at MAX_BOOKS_PER_DAY; a day with more distinct
// books keeps the totals exact and flags "books_truncated":true. A book id that
// does not fit BOOK_ID_CAP (escaped, as stored in the history line) is left out
// the same way rather than stored truncated — two long paths cut at the same
// byte would silently merge into one entry.
//
// Pure C++ (no SD, no ESP-IDF) so test/reading_session compiles the same code
// the firmware runs.
namespace ReadingHistory {

inline constexpr const char* AGGREGATE_DIR = "/.crosspoint/aggregate";
inline constexpr size_t MAX_BOOKS_PER_DAY = 8;
inline constexpr size_t BOOK_ID_CAP = 160;
// One serialized day with all book shares: 8 × (~id + 40) + totals ≈ 1.6 KB.
inline constexpr size_t DAY_LINE_CAP = 1792;
// RH-03 shows 12 weeks (84 days) in the GitHub-contribution style grid.
inline constexpr int HEATMAP_WEEKS = 12;
inline constexpr int HEATMAP_DAYS = HEATMAP_WEEKS * 7;

struct BookShare {
  // JSON-escaped book id exactly as the history writer stored it, so the
  // aggregate line can embed it without a decode/encode round trip.
  char id[BOOK_ID_CAP] = {};
  uint32_t sec = 0;
};

struct DayAggregate {
  int32_t dayIndex = 0;  // local days since 1970-01-01
  uint32_t totalSec = 0;
  uint32_t pagesAdvanced = 0;
  uint16_t sessions = 0;
  uint8_t bookCount = 0;
  bool booksTruncated = false;
  BookShare books[MAX_BOOKS_PER_DAY];
};

// One day as read back from the aggregate file (books are not needed by the
// heatmap, so they are not decoded into RAM).
struct DayStat {
  int32_t dayIndex = 0;
  uint32_t totalSec = 0;
  uint32_t pagesAdvanced = 0;
  uint16_t sessions = 0;
  bool present = false;
};

// Heatmap shading (v4.0 RH-03): 0 none, 1 <15 min, 2 15–40 min, 3 >40 min.
inline constexpr uint32_t HEAT_LEVEL_1_SEC = 15 * 60;
inline constexpr uint32_t HEAT_LEVEL_2_SEC = 40 * 60;

inline uint8_t heatLevel(const uint32_t totalSec) {
  if (totalSec == 0) return 0;
  if (totalSec < HEAT_LEVEL_1_SEC) return 1;
  if (totalSec < HEAT_LEVEL_2_SEC) return 2;
  return 3;
}

// ISO weekday of a days-since-epoch index: Monday = 1 … Sunday = 7.
inline int isoWeekdayFromDayIndex(const int32_t dayIndex) {
  // 1970-01-01 (index 0) was a Thursday; (r + 3) % 7 maps that to Mon = 1.
  const int r = static_cast<int>(((dayIndex % 7) + 7) % 7);
  return ((r + 3) % 7) + 1;
}

// Monday of the week containing `dayIndex`.
inline int32_t weekStartMonday(const int32_t dayIndex) { return dayIndex - (isoWeekdayFromDayIndex(dayIndex) - 1); }

// First day of the RH-03 window: HEATMAP_WEEKS Monday-anchored columns ending
// with the week that contains `todayDayIndex` (the trailing future days of
// that week are simply empty).
inline int32_t heatmapFirstDayIndex(const int32_t todayDayIndex) {
  return weekStartMonday(todayDayIndex) - (HEATMAP_WEEKS - 1) * 7;
}

// "YYYY-MM-DD" for a days-since-epoch index.
inline bool formatDayDate(char* out, const size_t cap, const int32_t dayIndex) {
  if (out == nullptr || cap < 11) return false;
  int year = 0;
  int month = 0;
  int day = 0;
  ChatLog::ChatHistoryFormat::civilFromDays(dayIndex, year, month, day);
  const int n = snprintf(out, cap, "%04d-%02d-%02d", year, month, day);
  return n > 0 && static_cast<size_t>(n) < cap;
}

// "YYYY-MM-DD.jsonl" for a days-since-epoch index (the history file name).
inline bool formatDayFileName(char* out, const size_t cap, const int32_t dayIndex) {
  if (out == nullptr || cap < 17) return false;
  int year = 0;
  int month = 0;
  int day = 0;
  ChatLog::ChatHistoryFormat::civilFromDays(dayIndex, year, month, day);
  const int n = snprintf(out, cap, "%04d-%02d-%02d.jsonl", year, month, day);
  return n > 0 && static_cast<size_t>(n) < cap;
}

// "/.crosspoint/aggregate/YYYY.json".
inline bool formatYearPath(char* out, const size_t cap, const int year) {
  if (out == nullptr) return false;
  const int n = snprintf(out, cap, "%s/%04d.json", AGGREGATE_DIR, year);
  return n > 0 && static_cast<size_t>(n) < cap;
}

// Value position of `"key"` in a line: after the colon and any spaces.
// Header lines in our writer use no spaces, but tolerating them keeps a
// hand-edited aggregate file readable. Returns nullptr when absent.
inline const char* jsonFieldValue(const char* line, const char* key) {
  if (line == nullptr || key == nullptr) return nullptr;
  const char* p = strstr(line, key);
  if (p == nullptr) return nullptr;
  p += strlen(key);
  while (*p == ' ') ++p;
  if (*p != ':') return nullptr;
  ++p;
  while (*p == ' ') ++p;
  return p;
}

inline bool jsonUintAt(const char* value, uint32_t& out) {
  if (value == nullptr || *value < '0' || *value > '9') return false;
  uint32_t v = 0;
  const char* p = value;
  while (*p >= '0' && *p <= '9') {
    const uint32_t digit = static_cast<uint32_t>(*p - '0');
    if (v > (UINT32_MAX - digit) / 10) return false;  // overflow
    v = v * 10 + digit;
    ++p;
  }
  out = v;
  return true;
}

// Numeric field `"key":123` from a history or aggregate line.
inline bool jsonUintField(const char* line, const char* key, uint32_t& out) {
  return jsonUintAt(jsonFieldValue(line, key), out);
}

// Raw content of string field `"key":"..."`. Escapes are copied through, not
// decoded: the aggregate re-emits the same escaped bytes. Returns false when
// the field is absent, not a string, or longer than cap-1.
inline bool jsonEscapedStringField(const char* line, const char* key, char* out, const size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  const char* p = jsonFieldValue(line, key);
  if (p == nullptr || *p != '"') return false;
  ++p;
  size_t w = 0;
  while (*p != '\0' && *p != '"') {
    if (*p == '\\' && p[1] != '\0') {
      if (w + 2 >= cap) return false;  // an escape only lands whole
      out[w++] = *p++;
      out[w++] = *p++;
      continue;
    }
    if (w + 1 >= cap) return false;
    out[w++] = *p++;
  }
  if (*p != '"') return false;
  out[w] = '\0';
  return true;
}

// Fold one history session into the day. `escapedBookId` is the raw value from
// the history line (empty when the record has no book), so identity is exact.
inline void addSession(DayAggregate& day, const uint32_t durationSec, const uint32_t pagesAdvanced,
                       const char* escapedBookId) {
  ++day.sessions;
  day.totalSec += durationSec;
  day.pagesAdvanced += pagesAdvanced;
  if (escapedBookId == nullptr || escapedBookId[0] == '\0') return;

  for (size_t i = 0; i < day.bookCount; ++i) {
    if (strcmp(day.books[i].id, escapedBookId) == 0) {
      day.books[i].sec += durationSec;
      return;
    }
  }
  if (day.bookCount >= MAX_BOOKS_PER_DAY || strlen(escapedBookId) >= BOOK_ID_CAP) {
    day.booksTruncated = true;
    return;
  }
  strcpy(day.books[day.bookCount].id, escapedBookId);
  day.books[day.bookCount].sec = durationSec;
  ++day.bookCount;
}

// One day record, no trailing newline (the year-file writer owns separators).
// Returns 0 when the line (or even the totals-only prefix) does not fit.
inline size_t formatDayLine(char* out, const size_t cap, const DayAggregate& day) {
  if (out == nullptr || cap < 64) return 0;
  char date[16];
  if (!formatDayDate(date, sizeof(date), day.dayIndex)) return 0;

  int n = snprintf(out, cap, "{\"date\":\"%s\",\"total_sec\":%u,\"sessions\":%u,\"pages_advanced\":%u,\"books\":[",
                   date, static_cast<unsigned>(day.totalSec), static_cast<unsigned>(day.sessions),
                   static_cast<unsigned>(day.pagesAdvanced));
  if (n <= 0 || static_cast<size_t>(n) >= cap) return 0;
  size_t w = static_cast<size_t>(n);

  size_t booksWritten = 0;
  for (size_t i = 0; i < day.bookCount; ++i) {
    n = snprintf(out + w, cap - w, "%s{\"id\":\"%s\",\"sec\":%u}", booksWritten > 0 ? "," : "", day.books[i].id,
                 static_cast<unsigned>(day.books[i].sec));
    if (n <= 0 || static_cast<size_t>(n) >= cap - w) break;  // keep totals; drop the rest
    w += static_cast<size_t>(n);
    ++booksWritten;
  }

  n = snprintf(out + w, cap - w, "]%s}", day.booksTruncated ? ",\"books_truncated\":true" : "");
  if (n <= 0 || static_cast<size_t>(n) >= cap - w) return 0;
  return w + static_cast<size_t>(n);
}

// Length of the day record inside a line, with the separator comma (and any
// stray spaces/CR) the previous write left at the end stripped. The writer
// emits ",\n" BEFORE each following line, so every line but the last in the
// file ends with the JSON separator; copying that comma verbatim would grow
// one extra comma per rewrite.
inline size_t dayLineRecordLength(const char* line) {
  if (line == nullptr) return 0;
  size_t len = strlen(line);
  while (len > 0) {
    const char c = line[len - 1];
    if (c == ',' || c == ' ' || c == '\t' || c == '\r') {
      --len;
    } else {
      break;
    }
  }
  return len;
}

// Defined below; emitMergedDays needs it to compare dates.
inline bool parseDayLineStat(const char* line, DayStat& out);

// Fetches the next existing day line (NUL-terminated) or nullptr at EOF.
using DayLineSource = const char* (*)(void* ctx);
// Receives one output piece; false aborts the merge.
using DayLineSink = bool (*)(void* ctx, const char* data, size_t len);

// Merge `dayLine` into an ordered stream of existing aggregate lines and emit
// the body of the "days" array (no header, no tail). Existing lines may end
// with the separator comma a previous write left there; it is stripped before
// re-emitting. Non-day lines are skipped. Returns false when the sink fails.
//
// Pure (callbacks in, bytes out) so the host tests drive the same merge the SD
// writer uses — the comma accumulation this replaced was invisible to the
// line-format tests alone.
inline bool emitMergedDays(const DayAggregate& day, const char* dayLine, const size_t dayLineLen,
                           DayLineSource nextLine, void* lineCtx, DayLineSink write, void* writeCtx) {
  if (dayLine == nullptr || dayLineLen == 0 || nextLine == nullptr || write == nullptr) return false;

  bool anyWritten = false;
  bool inserted = false;
  const auto emit = [&](const char* text, const size_t len) -> bool {
    if (anyWritten && !write(writeCtx, ",\n", 2)) return false;
    if (len > 0 && !write(writeCtx, text, len)) return false;
    anyWritten = true;
    return true;
  };

  for (const char* line = nextLine(lineCtx); line != nullptr; line = nextLine(lineCtx)) {
    if (strncmp(line, "{\"date\":", 8) != 0) continue;  // header/tail/foreign line
    DayStat existing;
    if (!parseDayLineStat(line, existing)) continue;  // unparsable: dropped on rewrite

    if (existing.dayIndex == day.dayIndex) {
      if (!emit(dayLine, dayLineLen)) return false;
      inserted = true;
      continue;
    }
    if (!inserted && day.dayIndex < existing.dayIndex) {
      if (!emit(dayLine, dayLineLen)) return false;
      inserted = true;
    }
    if (!emit(line, dayLineRecordLength(line))) return false;
  }

  if (!inserted && !emit(dayLine, dayLineLen)) return false;
  return true;
}

// Parse the date + totals of one aggregate day line. Book shares are skipped.
inline bool parseDayLineStat(const char* line, DayStat& out) {
  out = DayStat{};
  if (line == nullptr) return false;
  char date[16];
  if (!jsonEscapedStringField(line, "\"date\"", date, sizeof(date))) return false;
  int year = 0;
  int month = 0;
  int day = 0;
  if (!ChatLog::ChatHistoryFormat::parseDayDate(date, year, month, day)) return false;
  out.dayIndex = static_cast<int32_t>(ChatLog::ChatHistoryFormat::daysFromCivil(year, month, day));
  out.present = true;
  uint32_t v = 0;
  if (jsonUintField(line, "\"total_sec\"", v)) out.totalSec = v;
  if (jsonUintField(line, "\"sessions\"", v)) out.sessions = static_cast<uint16_t>(v > UINT16_MAX ? UINT16_MAX : v);
  if (jsonUintField(line, "\"pages_advanced\"", v)) out.pagesAdvanced = v;
  return true;
}

}  // namespace ReadingHistory
