#pragma once

#include <cstddef>
#include <cstdint>

#include "SyncCodec.h"

// ESP-IDF transport for POST /api/v1/sync/push. The wire bytes/parsing live in
// SyncCodec; this file only owns the blocking HTTP round trip.
//
// Buffers are PSRAM-first: a full 50-op batch can carry AI results with their
// segment/citation JSON, so the request is up to 128 KB and never belongs on
// the internal heap across a TLS handshake.
namespace Sync {

class Client {
 public:
  enum class Result : uint8_t {
    Ok,
    BadConfig,
    NoWifi,
    Time,
    Connect,
    Http,
    Auth,
    Upstream,
    Read,
    Oom,
    Protocol,  // 200 but the body did not parse
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
  };

  static Result push(const char* url, const char* token, const char* deviceId, const char* const* ops, size_t count,
                     PushResult* results, size_t maxResults, bool& allAcked, Stats* stats = nullptr);

  static const char* resultName(Result result);
};

}  // namespace Sync
