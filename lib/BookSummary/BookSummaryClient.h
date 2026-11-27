#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "BookSummaryCodec.h"

namespace BookSummary {

class Client {
 public:
  static constexpr uint32_t TOTAL_TIMEOUT_MS = 60000;

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
    NotReady,
    Upstream,
    Read,
    Oom,
    Parse,
    Cancelled,
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
  };

  static Result get(const char* url, const char* token, const char* deviceId, const char* bookId, bool wholeBook,
                    int spineIndex, Response& response, Stats* stats = nullptr,
                    const std::atomic<bool>* cancel = nullptr);

  static const char* resultName(Result result);
};

}  // namespace BookSummary
