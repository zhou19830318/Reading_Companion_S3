#pragma once

#include <cstddef>
#include <cstdint>

// `/api/v1/sync/push` wire codec (docs/ai-native/api-v1.md §5.2). Pure C++ so
// test/sync_codec can exercise it on the host; the ESP-IDF transport lives in
// SyncClient.
namespace Sync {

inline constexpr size_t OP_ID_CAP = 40;
inline constexpr size_t MAX_RESULTS = 64;

enum class PushStatus : uint8_t {
  Applied,
  Duplicate,
  Rejected,
  Conflict,
  Unknown,
};

struct PushResult {
  char opId[OP_ID_CAP] = {};
  PushStatus status = PushStatus::Unknown;
};

// Builds one batch body. `ops` are the raw JSON objects from the outbox (the
// "SYO1 " prefix already stripped). Returns the byte length or 0 when the
// batch is empty/oversized or an argument is invalid.
size_t buildPushRequest(char* out, size_t cap, const char* deviceId, const char* batchId, const char* const* ops,
                        size_t count);

// Parses the push response into `results`. `allAcked` is true only when every
// operation came back applied/duplicate (safe to clear the outbox). Returns the
// number of results parsed, or 0 on malformed JSON.
size_t parsePushResponse(const char* json, size_t len, PushResult* results, size_t maxResults, size_t expectedOps,
                         bool& allAcked);

// Builds one `note.create` operation (the outbox stores it without the
// "SYO1 " prefix). The server accepts an empty passage_id/anchor for a note
// that was captured on a page rather than a mapped paragraph, so a voice note
// from an unindexed book still synchronises. Returns the byte length or 0 when
// the buffer is too small or a required field is missing.
size_t buildNoteCreateOp(char* out, size_t cap, const char* opId, const char* entityId, const char* bookId,
                         const char* passageId, const char* quote, const char* note, const char* kind,
                         uint16_t spineIndex, uint16_t pageNumber, uint16_t pageCount, uint64_t clientTs);

const char* pushStatusName(PushStatus status);

}  // namespace Sync
