#pragma once

#include <ChatHistoryFormat.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ReadingHistory {

inline constexpr const char* DIR = "/.crosspoint/history";

// Escape a UTF-8 string for a JSON string value. Writes into `out` with NUL.
// Returns bytes written (excluding NUL), or 0 if it did not fit.
inline size_t escapeJsonString(char* out, const size_t cap, const char* in) {
  if (out == nullptr || in == nullptr || cap == 0) return 0;
  size_t w = 0;
  for (const char* p = in; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const char* esc = nullptr;
    char buf[7] = {};
    if (c == '"')
      esc = "\\\"";
    else if (c == '\\')
      esc = "\\\\";
    else if (c == '\n')
      esc = "\\n";
    else if (c == '\r')
      esc = "\\r";
    else if (c == '\t')
      esc = "\\t";
    else if (c < 0x20) {
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      esc = buf;
    }
    const size_t need = esc ? strlen(esc) : 1;
    if (w + need + 1 > cap) return 0;  // +1 for NUL
    if (esc) {
      memcpy(out + w, esc, need);
    } else {
      out[w] = *p;
    }
    w += need;
  }
  out[w] = '\0';
  return w;
}

// Format an ISO-8601 local timestamp with the biased quarter-hour offset.
// Returns true on success.
inline bool formatIsoTimestamp(char* out, const size_t cap, const int64_t epochSec,
                               const int utcOffsetQuarterHoursBiased) {
  if (out == nullptr || cap < 20) return false;
  if (epochSec < ChatLog::MIN_SANE_EPOCH_MS / 1000) return false;
  const int64_t localSec =
      ChatLog::ChatHistoryFormat::localEpochSeconds(static_cast<int64_t>(epochSec) * 1000, utcOffsetQuarterHoursBiased);
  const int64_t days = localSec >= 0 ? localSec / 86400 : (localSec - 86399) / 86400;
  int year = 0, month = 0, day = 0;
  ChatLog::ChatHistoryFormat::civilFromDays(days, year, month, day);
  const int64_t secsOfDay = ((localSec % 86400) + 86400) % 86400;
  const int h = static_cast<int>(secsOfDay / 3600);
  const int m = static_cast<int>((secsOfDay / 60) % 60);
  const int s = static_cast<int>(secsOfDay % 60);
  const int q = utcOffsetQuarterHoursBiased - 48;
  const int offH = (q >= 0 ? q : -q) / 4;
  const int offM = ((q >= 0 ? q : -q) % 4) * 15;
  const int n = snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", year, month, day, h, m, s,
                         q >= 0 ? '+' : '-', offH, offM);
  return n > 0 && static_cast<size_t>(n) < cap;
}

// Format one reading session as a single JSONL record. Returns bytes written
// (excluding NUL), or 0 on failure/truncation. The record is always terminated
// with '\n' when it fits.
inline size_t formatSessionLine(char* out, const size_t cap, const char* bookPath, const int spineIndex,
                                const int pageNumber, const uint32_t pagesAdvanced, const uint32_t durationSec,
                                const int64_t tsStartSec, const int64_t tsEndSec,
                                const int utcOffsetQuarterHoursBiased) {
  if (out == nullptr || cap < 64 || bookPath == nullptr) return 0;

  char escaped[256];
  // escapeJsonString returns 0 for an empty input as well as for truncation;
  // an empty book path is allowed, a truncated one is not.
  if (escapeJsonString(escaped, sizeof(escaped), bookPath) == 0 && bookPath[0] != '\0') return 0;

  char startBuf[32] = "";
  char endBuf[32] = "";
  if (!formatIsoTimestamp(startBuf, sizeof(startBuf), tsStartSec, utcOffsetQuarterHoursBiased)) return 0;
  if (!formatIsoTimestamp(endBuf, sizeof(endBuf), tsEndSec, utcOffsetQuarterHoursBiased)) return 0;

  const int n = snprintf(out, cap,
                         "{\"schema_version\":1,\"ts_start\":\"%s\",\"ts_end\":\"%s\",\"book_id\":\"%s\","
                         "\"position\":{\"spine\":%d,\"page\":%d},\"duration_sec\":%u,\"pages_advanced\":%u}\n",
                         startBuf, endBuf, escaped, spineIndex, pageNumber, static_cast<unsigned>(durationSec),
                         static_cast<unsigned>(pagesAdvanced));
  if (n <= 0 || static_cast<size_t>(n) >= cap) return 0;
  return static_cast<size_t>(n);
}

}  // namespace ReadingHistory
