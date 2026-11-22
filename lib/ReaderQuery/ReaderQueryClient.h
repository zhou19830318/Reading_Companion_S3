#pragma once

#include <cstddef>
#include <cstdint>

#include "ReaderQueryCodec.h"

// One blocking POST /api/v1/reader/query on the configured relay origin.
// The wire bytes and the response model live in ReaderQueryCodec (host-
// testable); this file is only the ESP-IDF transport.
//
// Blocking by design: the Ask-AI screen paints its waiting frame first and
// runs one question at a time, exactly like the /chat client. The caller
// supplies a PSRAM-allocated Response (~16 KB) and keeps it for the answer
// screen; this function never allocates the model itself.
namespace ReaderQuery {

class Client {
 public:
  enum class Result : uint8_t {
    Ok,
    BadConfig,  // url/token/device missing or the request body did not fit
    NoWifi,
    Time,  // TLS needs a sane clock and SNTP did not settle
    Connect,
    Http,
    Auth,      // 401 or UNAUTHORIZED / DEVICE_NOT_ALLOWED
    Upstream,  // provider-class failure (LLM_*, overload, rate limit)
    Read,
    Oom,
    Stale,       // echoed request_id differs (a late response from another round)
    NoSegments,  // 200 without a usable segment
    NotIndexed,  // the relay does not know this book (or its passages)
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
    char requestId[REQUEST_ID_CAP] = {};
  };

  // `url` is the configured relay URL (ws:// or wss://); the same origin's
  // /api/v1/reader/query is used. The request's requestId/question pointers
  // are copied into the body immediately and may be freed on return.
  static Result ask(const char* url, const char* token, const char* deviceId, const Request& request,
                    Response& response, Stats* stats = nullptr);

  static const char* resultName(Result result);
};

}  // namespace ReaderQuery
