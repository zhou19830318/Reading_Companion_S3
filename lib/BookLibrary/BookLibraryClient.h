#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "BookLibraryCodec.h"

namespace BookLibrary {

class Client {
 public:
  static constexpr uint32_t TOTAL_TIMEOUT_MS = 10 * 60 * 1000;

  enum class Result : uint8_t {
    Ok,
    BadConfig,
    NoWifi,
    Time,
    Connect,
    Timeout,
    Http,
    Auth,
    NotFound,
    TooLarge,
    Unsupported,
    Upstream,
    IngestFailed,
    Read,
    Oom,
    Parse,
    Cancelled,
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
  };

  struct Progress {
    bool uploading = true;
    uint8_t percent = 0;
  };

  using ProgressCallback = void (*)(void* ctx, const Progress& progress);

  static Result ensureReady(const char* url, const char* token, const char* deviceId, const char* bookId,
                            const char* bookPath, bool upload, ProgressCallback onProgress, void* ctx,
                            IngestResponse& response, Stats* stats = nullptr,
                            const std::atomic<bool>* cancel = nullptr);

  static const char* resultName(Result result);
};

}  // namespace BookLibrary
