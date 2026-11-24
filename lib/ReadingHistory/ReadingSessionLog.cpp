#include "ReadingSessionLog.h"

#include <ChatHistoryFormat.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "ReadingAggregate.h"
#include "ReadingSessionFormat.h"

namespace ReadingHistory {

namespace {
constexpr size_t PATH_CAP = 64;
constexpr size_t LINE_CAP = 512;
}  // namespace

bool appendSession(const SessionRecord& session, const char* bookPath, int spineIndex, int pageNumber,
                   int utcOffsetQuarterHoursBiased) {
  if (!session.valid()) {
    LOG_ERR("RH", "refusing invalid session (%lld..%lld)", static_cast<long long>(session.startSec),
            static_cast<long long>(session.endSec));
    return false;
  }

  char dayName[ChatLog::ChatHistoryFormat::DATE_PATH_SIZE];
  // dayPath takes epoch ms; our session timestamps are seconds.
  if (!ChatLog::ChatHistoryFormat::dayPath(dayName, session.startSec * 1000, utcOffsetQuarterHoursBiased)) {
    LOG_ERR("RH", "invalid start time for session");
    return false;
  }
  // dayPath returns "YYYY-MM-DD.jsonl"; keep that file name as-is.

  if (!Storage.ensureDirectoryExists(DIR)) {
    LOG_ERR("RH", "mkdir failed: %s", DIR);
    return false;
  }

  char path[PATH_CAP];
  const int n = snprintf(path, sizeof(path), "%s/%s", DIR, dayName);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) {
    LOG_ERR("RH", "session path too long");
    return false;
  }

  const uint32_t durationSec = static_cast<uint32_t>(session.endSec - session.startSec);
  char line[LINE_CAP];
  const size_t len = formatSessionLine(line, sizeof(line), bookPath == nullptr ? "" : bookPath, spineIndex, pageNumber,
                                       session.pagesAdvanced, durationSec, session.startSec, session.endSec,
                                       utcOffsetQuarterHoursBiased);
  if (len == 0) {
    LOG_ERR("RH", "session line did not fit");
    return false;
  }

  {
    HalFile f = Storage.open(path, O_WRITE | O_APPEND | O_CREAT);
    if (!f.isOpen()) {
      LOG_ERR("RH", "open for append failed: %s", path);
      return false;
    }
    const size_t written = f.write(line, len);
    if (written != len) {
      LOG_ERR("RH", "short write %u/%u: %s", static_cast<unsigned>(written), static_cast<unsigned>(len), path);
      return false;
    }
    // Close before the aggregate step reads the same file: a handle left open
    // for append may still hold the last sector in SdFat's write buffer.
  }
  LOG_DBG("RH", "session saved (%u bytes) -> %s", static_cast<unsigned>(len), path);
  // Debug aid: the full record proves duration/pages without pulling the SD card.
  LOG_DBG("RH", "record: %.*s", static_cast<int>(len > 0 ? len - 1 : 0), line);

  // RH-02: refresh today's aggregate from the history file just appended to.
  // Best effort: the session record is already durable, and the stats view can
  // recompute a missing aggregate day from the same history file.
  if (!updateDayAggregate(dayName)) LOG_ERR("RH", "aggregate update failed for %s", dayName);
  return true;
}

}  // namespace ReadingHistory
