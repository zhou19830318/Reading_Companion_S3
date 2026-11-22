#include "SyncRunner.h"

#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include "SyncOutbox.h"

namespace Sync {
namespace {

// One batch of 50 AI results with their citation JSON fits well inside this.
constexpr size_t BATCH_CAP = 128 * 1024;
// 512-line outbox / 50-op batches = 11 rounds; leave slack for retries.
constexpr size_t MAX_ROUNDS = 16;

struct HeapCapsDeleter {
  void operator()(char* p) const { heap_caps_free(p); }
};
using Scratch = std::unique_ptr<char[], HeapCapsDeleter>;

Scratch allocScratch(const size_t size) {
  char* p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (p == nullptr) p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return Scratch(p);
}

}  // namespace

RunSummary pushAll(const char* url, const char* token, const char* deviceId) {
  RunSummary sum;
  if (url == nullptr || token == nullptr || deviceId == nullptr) {
    sum.result = Client::Result::BadConfig;
    return sum;
  }

  Scratch batch = allocScratch(BATCH_CAP);
  auto results = makeUniqueNoThrow<PushResult[]>(MAX_BATCH_OPS);
  if (!batch || !results) {
    LOG_ERR("SYNR", "OOM: batch %u + results", static_cast<unsigned>(BATCH_CAP));
    sum.result = Client::Result::Oom;
    sum.remaining = stats().lines;
    return sum;
  }

  const char* ops[MAX_BATCH_OPS] = {};
  for (size_t round = 0; round < MAX_ROUNDS; ++round) {
    const size_t count = loadBatch(batch.get(), BATCH_CAP, ops, MAX_BATCH_OPS);
    if (count == 0) break;

    bool allAcked = false;
    Client::Stats httpStats;
    sum.result = Client::push(url, token, deviceId, ops, count, results.get(), MAX_BATCH_OPS, allAcked, &httpStats);
    sum.httpStatus = httpStats.httpStatus;
    sum.batches++;
    if (sum.result != Client::Result::Ok) {
      LOG_ERR("SYNR", "push stopped: %s (http=%d, %u ops)", Client::resultName(sum.result), sum.httpStatus,
              static_cast<unsigned>(count));
      break;
    }

    if (!allAcked) {
      for (size_t i = 0; i < count; ++i) {
        if (results[i].status == PushStatus::Rejected || results[i].status == PushStatus::Conflict) {
          sum.failed++;
          if (results[i].status == PushStatus::Rejected) sum.rejected++;
          if (results[i].status == PushStatus::Conflict) sum.conflict++;
          if (sum.firstFailedOpId[0] == '\0') {
            std::snprintf(sum.firstFailedOpId, sizeof(sum.firstFailedOpId), "%s", results[i].opId);
            sum.firstFailedStatus = results[i].status;
          }
        }
      }
      LOG_ERR("SYNR", "batch left queued: %u rejected/conflict of %u", static_cast<unsigned>(sum.failed),
              static_cast<unsigned>(count));
      break;
    }

    sum.acked += count;
    if (!removeFront(count)) {
      LOG_ERR("SYNR", "removeFront(%u) failed; kept outbox for retry", static_cast<unsigned>(count));
      break;
    }
    if (stats().lines == 0) break;
    // Give the render/input tasks a slot between synchronous HTTP rounds.
    vTaskDelay(1);
  }

  sum.remaining = stats().lines;
  LOG_INF("SYNR", "done: %s batches=%u acked=%u failed=%u remaining=%u", Client::resultName(sum.result),
          static_cast<unsigned>(sum.batches), static_cast<unsigned>(sum.acked), static_cast<unsigned>(sum.failed),
          static_cast<unsigned>(sum.remaining));
  return sum;
}

}  // namespace Sync
