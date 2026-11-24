#pragma once

#include <ReadingSessionTracker.h>

#include <cstdint>

namespace ReadingHistory {

// Append one session record to /.crosspoint/history/<day>.jsonl.
// Returns true on success. bookPath may be nullptr/empty (stored as "").
bool appendSession(const SessionRecord& session, const char* bookPath, int spineIndex, int pageNumber,
                   int utcOffsetQuarterHoursBiased);

}  // namespace ReadingHistory
