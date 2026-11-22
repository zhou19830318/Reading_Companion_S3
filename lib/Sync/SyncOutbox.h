#pragma once

#include <cstddef>
#include <cstdint>

// Device-side sync outbox (docs/ai-native/api-v1.md §5).
//
// One operation per line:
//   SYO1 {"op_id":"...","type":"ai_result.create",...}\n
//
// The "SYO1 " prefix is a format tag (version + a shape a stray text file in
// the same directory cannot match). The JSON after it is exactly one entry of
// the server's `operations` array; the push batch wraps the lines as-is.
//
// Caps: 512 operations / 256 KB. Append refuses past the cap instead of
// silently dropping a record; the sync screen surfaces the count so the user
// can push/clear. V1 only enqueues AI results; annotations follow the same
// shape.
namespace Sync {

inline constexpr const char* OUTBOX_DIR = "/.crosspoint/sync";
inline constexpr const char* OUTBOX_PATH = "/.crosspoint/sync/outbox.syo1";
inline constexpr char LINE_PREFIX[] = "SYO1 ";
inline constexpr size_t LINE_PREFIX_LEN = 5;
inline constexpr size_t MAX_LINES = 512;
inline constexpr size_t MAX_BYTES = 256 * 1024;
inline constexpr size_t MAX_BATCH_OPS = 50;    // server sync_batch_max_ops
inline constexpr size_t MAX_OP_BYTES = 12288;  // one AI result with citations

struct Stats {
  uint16_t lines = 0;
  uint32_t bytes = 0;
};

// Current outbox size. Reads the file; cheap at the caps above.
Stats stats();

// Appends one operation JSON object (no prefix/newline). Returns false when
// the object is too large, the outbox is full, or the SD write fails.
bool append(const char* opJson, size_t len);

// Removes every queued operation. Called after a push whose results were all
// applied/duplicate.
bool clear();

// Removes the first `lines` queued operations (the batch the server just
// acked) and keeps the remainder for the next push. Needed because one push
// carries at most 50 ops: clearing after a full batch would silently drop the
// rest. Rewrites through a temp file so a crash mid-rewrite cannot leave a
// truncated outbox; removes the file when the last line is consumed.
bool removeFront(size_t lines);

// Reads the first `maxOps` complete lines into `buf` (NUL-terminated) and
// points `ops` at each JSON object. A trailing partial line is dropped.
// Returns the operation count (0 when the outbox is empty/unreadable).
size_t loadBatch(char* buf, size_t cap, const char* ops[], size_t maxOps);

}  // namespace Sync
