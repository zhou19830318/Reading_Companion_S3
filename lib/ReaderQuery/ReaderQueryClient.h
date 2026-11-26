#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "ReaderQueryCodec.h"

// One POST /api/v1/reader/query on the configured relay origin. The call blocks
// its worker task; the Ask-AI screen runs it off the input loop so Back stays
// responsive. Wire bytes and the response model live in ReaderQueryCodec.
namespace ReaderQuery {

class Client {
 public:
  static constexpr uint32_t TOTAL_TIMEOUT_MS = 60000;

  enum class Result : uint8_t {
    Ok,
    BadConfig,  // url/token/device missing or the request body did not fit
    NoWifi,
    Time,  // TLS needs a sane clock and SNTP did not settle
    Connect,
    Timeout,
    Http,
    Auth,      // 401 or UNAUTHORIZED / DEVICE_NOT_ALLOWED
    Upstream,  // provider-class failure (LLM_*, overload, rate limit)
    Read,
    Oom,
    Stale,       // echoed request_id differs (a late response from another round)
    Cancelled,   // caller set the optional cancellation flag
    NoSegments,  // 200 without a usable segment
    // Library states that still get an uncited /chat answer, each with its own
    // notice so the reader can tell "not uploaded" from "still indexing".
    NotUploaded,         // BOOK_NOT_FOUND
    Indexing,            // BOOK_NOT_READY
    PassageMissing,      // PASSAGE_NOT_FOUND
    LibraryUnavailable,  // route-level 404: relay older than the library API
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
    char requestId[REQUEST_ID_CAP] = {};
  };

  // `url` is the configured relay URL (ws:// or wss://); the same origin's
  // /api/v1/reader/query is used. The request's requestId/question pointers
  // are copied into the body immediately and may be freed on return.
  // `cancel` is optional and may be set by another task. It is checked around
  // blocking socket operations; an in-flight operation still ends at the HTTP
  // socket timeout. A cancelled result must never update the visible answer.
  static Result ask(const char* url, const char* token, const char* deviceId, const Request& request,
                    Response& response, Stats* stats = nullptr, const std::atomic<bool>* cancel = nullptr);

  static const char* resultName(Result result);
};

}  // namespace ReaderQuery
