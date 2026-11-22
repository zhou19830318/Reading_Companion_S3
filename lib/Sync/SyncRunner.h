#pragma once

#include <cstddef>
#include <cstdint>

#include "SyncClient.h"

// Drains the device outbox through `/api/v1/sync/push`.
//
// One request carries at most `MAX_BATCH_OPS` (50) operations, while the
// outbox caps at 512 lines. A caller that pushed a single batch and then called
// outbox clear() would silently discard the rest, so the batch loop lives here
// (used by both the sync screen and the SYNC_PUSH serial command).
namespace Sync {

struct RunSummary {
  Client::Result result = Client::Result::Ok;
  int httpStatus = 0;
  size_t batches = 0;
  size_t acked = 0;     // operations accepted by the server (applied/duplicate)
  size_t failed = 0;    // rejected/conflict in the last batch
  size_t rejected = 0;  // invalid operation the server refused
  size_t conflict = 0;  // same entity edited elsewhere; kept for the user
  char firstFailedOpId[OP_ID_CAP] = {};
  PushStatus firstFailedStatus = PushStatus::Unknown;
  size_t remaining = 0;  // lines still queued afterwards
};

// Pushes every queued operation, removing acked lines as it goes. Stops at the
// first transport error or un-acked result and leaves the remainder queued for
// a retry (the server deduplicates by op_id). Blocking; call from a task that
// can afford a 30 s HTTP round per batch.
RunSummary pushAll(const char* url, const char* token, const char* deviceId);

}  // namespace Sync
