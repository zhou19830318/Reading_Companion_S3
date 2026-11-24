#include "ReadingAggregate.h"

#include <ChatHistoryFormat.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "ReadingSessionFormat.h"  // ReadingHistory::DIR (history/)

namespace ReadingHistory {
namespace {

constexpr size_t PATH_CAP = 72;

// Streams a file line by line without a DOM. Holds the line assembly buffer
// (~DAY_LINE_CAP) plus a read chunk, so instances are heap-allocated rather
// than put on the 8 KB activity/render stacks (CLAUDE.md stack rule).
class LineReader {
 public:
  explicit LineReader(HalFile& file) : file_(file) {}

  // Next line, NUL-terminated and stripped of CR/LF. nullptr at EOF.
  const char* next() {
    if (done_) return nullptr;
    lineLen_ = 0;
    truncated_ = false;
    while (true) {
      if (chunkPos_ >= chunkLen_) {
        const int r = file_.read(chunk_, sizeof(chunk_));
        if (r <= 0) {
          done_ = true;
          if (lineLen_ == 0) return nullptr;
          line_[lineLen_] = '\0';
          return line_;
        }
        chunkLen_ = r;
        chunkPos_ = 0;
      }
      const char c = chunk_[chunkPos_++];
      if (c == '\n') {
        while (lineLen_ > 0 && line_[lineLen_ - 1] == '\r') --lineLen_;
        line_[lineLen_] = '\0';
        return line_;
      }
      if (lineLen_ + 1 < sizeof(line_)) {
        line_[lineLen_++] = c;
      } else {
        // Keep consuming to the newline but do not hand a cut line to a parser:
        // a truncated JSON record could be mistaken for a valid one.
        truncated_ = true;
      }
    }
  }

  bool truncated() const { return truncated_; }

 private:
  HalFile& file_;
  char chunk_[256] = {};
  int chunkPos_ = 0;
  int chunkLen_ = 0;
  char line_[DAY_LINE_CAP] = {};
  size_t lineLen_ = 0;
  bool done_ = false;
  bool truncated_ = false;
};

bool isDayLine(const char* line) { return strncmp(line, "{\"date\":", 8) == 0; }

// Sum one day's history JSONL into `day`. Returns true when at least one valid
// session line was folded in.
bool aggregateHistoryDay(const char* historyPath, DayAggregate& day) {
  HalFile src;
  if (!Storage.openFileForRead("RH", historyPath, src)) return false;
  auto reader = makeUniqueNoThrow<LineReader>(src);
  if (!reader) {
    LOG_ERR("RH", "OOM: history line reader");
    return false;
  }

  bool any = false;
  for (const char* line = reader->next(); line != nullptr; line = reader->next()) {
    if (reader->truncated()) {
      LOG_ERR("RH", "history line too long, skipped: %s", historyPath);
      continue;
    }
    if (line[0] != '{') continue;
    uint32_t duration = 0;
    if (!jsonUintField(line, "\"duration_sec\"", duration)) continue;
    uint32_t pages = 0;
    jsonUintField(line, "\"pages_advanced\"", pages);
    char book[BOOK_ID_CAP];
    const bool hasBook = jsonEscapedStringField(line, "\"book_id\"", book, sizeof(book));
    addSession(day, duration, pages, hasBook ? book : "");
    any = true;
  }
  return any;
}

struct MergeCtx {
  LineReader* reader = nullptr;
  HalFile* file = nullptr;
};

const char* nextMergeLine(void* ctx) {
  auto* merge = static_cast<MergeCtx*>(ctx);
  for (const char* line = merge->reader->next(); line != nullptr; line = merge->reader->next()) {
    if (!merge->reader->truncated()) return line;
    LOG_ERR("RH", "aggregate line too long, dropped");
  }
  return nullptr;
}

const char* noMergeLine(void*) { return nullptr; }

bool writeMergeLine(void* ctx, const char* data, const size_t len) {
  HalFile* file = static_cast<MergeCtx*>(ctx)->file;
  return file->write(data, len) == len;
}

// Rebuild the year document with `day` inserted/replaced in date order.
bool upsertDayInYear(const char* yearPath, const char* tmpPath, const DayAggregate& day) {
  char target[DAY_LINE_CAP];
  const size_t targetLen = formatDayLine(target, sizeof(target), day);
  if (targetLen == 0) {
    LOG_ERR("RH", "aggregate day line did not fit");
    return false;
  }

  int year = 0;
  int month = 0;
  int dom = 0;
  ChatLog::ChatHistoryFormat::civilFromDays(day.dayIndex, year, month, dom);

  bool ok = true;
  {
    HalFile dst = Storage.open(tmpPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!dst.isOpen()) {
      LOG_ERR("RH", "open aggregate tmp failed: %s", tmpPath);
      return false;
    }

    char header[64];
    const int headerLen = snprintf(header, sizeof(header), "{\"schema_version\":1,\"year\":%d,\"days\":[\n", year);
    ok = headerLen > 0 && static_cast<size_t>(headerLen) < sizeof(header) &&
         dst.write(header, static_cast<size_t>(headerLen)) == static_cast<size_t>(headerLen);

    // The merge itself is pure (ReadingAggregateFormat.h) so the host tests
    // cover the comma/order rules; this layer only supplies the file streams.
    HalFile src;
    auto reader = makeUniqueNoThrow<LineReader>(src);
    if (!reader) {
      LOG_ERR("RH", "OOM: aggregate line reader");
      ok = false;
    }

    MergeCtx ctx;
    ctx.file = &dst;
    ctx.reader = reader.get();
    if (ok) {
      const bool haveSource = Storage.exists(yearPath) && Storage.openFileForRead("RH", yearPath, src);
      if (!haveSource) ctx.reader = nullptr;
      ok = emitMergedDays(day, target, targetLen, haveSource ? nextMergeLine : noMergeLine, &ctx, writeMergeLine, &ctx);
    }

    if (ok && dst.write("\n]}\n", 4) != 4) ok = false;
    if (ok) dst.flush();
  }  // both handles close here (DESTRUCTOR_CLOSES_FILE) before the rename

  if (!ok) {
    Storage.remove(tmpPath);
    LOG_ERR("RH", "aggregate write failed: %s", yearPath);
    return false;
  }

  // SdFat's rename does not overwrite, so drop the old file first. The brief
  // window with no year file reads as "no aggregate yet": the stats view then
  // falls back to the history files, so nothing user-visible is lost.
  Storage.remove(yearPath);
  if (!Storage.rename(tmpPath, yearPath)) {
    LOG_ERR("RH", "aggregate rename failed: %s", yearPath);
    return false;
  }
  return true;
}

}  // namespace

bool updateDayAggregate(const char* dayName) {
  if (dayName == nullptr) return false;
  const char* base = strrchr(dayName, '/');
  base = base != nullptr ? base + 1 : dayName;

  int year = 0;
  int month = 0;
  int dom = 0;
  if (!ChatLog::ChatHistoryFormat::parseDayDate(base, year, month, dom)) {
    LOG_ERR("RH", "bad day name: %s", dayName);
    return false;
  }

  auto day = makeUniqueNoThrow<DayAggregate>();
  if (!day) {
    LOG_ERR("RH", "OOM: day aggregate");
    return false;
  }
  day->dayIndex = static_cast<int32_t>(ChatLog::ChatHistoryFormat::daysFromCivil(year, month, dom));

  char historyPath[PATH_CAP];
  const int n = strstr(base, ".jsonl") != nullptr
                    ? snprintf(historyPath, sizeof(historyPath), "%s/%s", DIR, base)
                    : snprintf(historyPath, sizeof(historyPath), "%s/%s.jsonl", DIR, base);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(historyPath)) {
    LOG_ERR("RH", "history path too long: %s", base);
    return false;
  }
  if (!aggregateHistoryDay(historyPath, *day)) {
    LOG_ERR("RH", "no readable sessions in %s", historyPath);
    return false;
  }

  if (!Storage.ensureDirectoryExists(AGGREGATE_DIR)) {
    LOG_ERR("RH", "mkdir failed: %s", AGGREGATE_DIR);
    return false;
  }

  char yearPath[PATH_CAP];
  char tmpPath[PATH_CAP + 8];
  if (!formatYearPath(yearPath, sizeof(yearPath), year)) {
    LOG_ERR("RH", "aggregate path too long for year %d", year);
    return false;
  }
  const int tmpLen = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", yearPath);
  if (tmpLen <= 0 || static_cast<size_t>(tmpLen) >= sizeof(tmpPath)) {
    LOG_ERR("RH", "aggregate tmp path too long");
    return false;
  }

  if (!upsertDayInYear(yearPath, tmpPath, *day)) return false;
  LOG_DBG("RH", "aggregate day %s: %u s, %u pages, %u sessions", base, static_cast<unsigned>(day->totalSec),
          static_cast<unsigned>(day->pagesAdvanced), static_cast<unsigned>(day->sessions));
  return true;
}

int loadDayStats(const int32_t firstDayIndex, const int count, DayStat* out, const bool fallbackToHistory) {
  if (out == nullptr || count <= 0) return 0;
  for (int i = 0; i < count; ++i) {
    out[i] = DayStat{};
    out[i].dayIndex = firstDayIndex + i;
  }

  const int32_t lastDayIndex = firstDayIndex + count - 1;
  int firstYear = 0;
  int firstMonth = 0;
  int firstDom = 0;
  int lastYear = 0;
  int lastMonth = 0;
  int lastDom = 0;
  ChatLog::ChatHistoryFormat::civilFromDays(firstDayIndex, firstYear, firstMonth, firstDom);
  ChatLog::ChatHistoryFormat::civilFromDays(lastDayIndex, lastYear, lastMonth, lastDom);

  int found = 0;
  for (int year = firstYear; year <= lastYear; ++year) {
    char path[PATH_CAP];
    if (!formatYearPath(path, sizeof(path), year)) continue;
    // exists() first: a missing year file is normal (the window spans a year
    // boundary, or no session has been aggregated yet) and openFileForRead
    // would print one "File does not exist" line per missing file.
    if (!Storage.exists(path)) continue;
    HalFile src;
    if (!Storage.openFileForRead("RH", path, src)) continue;
    auto reader = makeUniqueNoThrow<LineReader>(src);
    if (!reader) {
      LOG_ERR("RH", "OOM: aggregate line reader");
      return found;
    }
    for (const char* line = reader->next(); line != nullptr; line = reader->next()) {
      if (reader->truncated() || !isDayLine(line)) continue;
      DayStat stat;
      if (!parseDayLineStat(line, stat)) continue;
      const int slot = stat.dayIndex - firstDayIndex;
      if (slot < 0 || slot >= count) continue;
      if (out[slot].present) continue;  // first file wins; years never overlap
      out[slot] = stat;
      ++found;
    }
  }

  if (!fallbackToHistory) return found;

  // RH-01 days recorded before RH-02 have no aggregate entry. Read them once
  // per view instead of rewriting the year file for every missing day.
  auto scratch = makeUniqueNoThrow<DayAggregate>();
  if (!scratch) {
    LOG_ERR("RH", "OOM: history fallback scratch");
    return found;
  }
  for (int i = 0; i < count; ++i) {
    if (out[i].present) continue;
    char name[ChatLog::ChatHistoryFormat::DATE_PATH_SIZE];
    if (!formatDayFileName(name, sizeof(name), out[i].dayIndex)) continue;
    char path[PATH_CAP];
    const int n = snprintf(path, sizeof(path), "%s/%s", DIR, name);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) continue;
    // Most days in a 12-week window have no session file at all; probing with
    // exists() keeps the serial log free of one "does not exist" per day.
    if (!Storage.exists(path)) continue;

    *scratch = DayAggregate{};
    scratch->dayIndex = out[i].dayIndex;
    if (!aggregateHistoryDay(path, *scratch)) continue;
    out[i].totalSec = scratch->totalSec;
    out[i].pagesAdvanced = scratch->pagesAdvanced;
    out[i].sessions = scratch->sessions;
    out[i].present = true;
    ++found;
  }
  return found;
}

}  // namespace ReadingHistory
