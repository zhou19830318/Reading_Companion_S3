#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// One-shot text round trip on the relay's /api/v1/chat endpoint. The call blocks
// its caller; Ask-AI uses a worker task so the input loop remains responsive.
// Host-testable request/response handling lives in VoiceRelayCodec.
namespace VoiceRelay {

class ChatClient {
 public:
  static constexpr uint32_t TOTAL_TIMEOUT_MS = 45000;

  enum class Result : uint8_t {
    Ok,
    BadConfig,  // url/token/device missing, request did not fit, or 404 (endpoint disabled)
    NoWifi,
    Time,  // TLS needs a sane clock and SNTP did not settle
    Connect,
    Timeout,
    Http,      // non-200 with no more specific class
    Auth,      // 401 or an UNAUTHORIZED / DEVICE_NOT_ALLOWED body
    Upstream,  // 502 / provider-class failure (LLM_*, overload, rate limit)
    Read,
    Cancelled,  // caller set the optional cancellation flag
    Oom,
    NoText,  // 200 without a text field
  };

  struct Stats {
    int httpStatus = 0;
    size_t responseBytes = 0;
  };

  // `url` is the configured relay URL (ws:// or wss://). On Ok, `reply` holds
  // the NUL-terminated reply text; `truncated` is set when it did not fit
  // replyCap (cut at a codepoint boundary) or the body outgrew the read
  // buffer. When strokesOut is non-null, the dict card's encoded stroke-order
  // payload (if the body carries one) lands there — always cleared on entry,
  // dropped whole on overflow. `mode` selects a server-side preset (nullptr =
  // plain ask, "dict" = dictionary entry). `stats` is optional.
  // `cancel` is optional and may be set by another task. Cancellation is
  // observed around socket operations; the current operation is bounded by the
  // HTTP timeout. Callers must discard the output when Cancelled is returned.
  static Result ask(const char* url, const char* token, const char* deviceId, const char* text, char* reply,
                    size_t replyCap, bool& truncated, char* strokesOut, size_t strokesCap, const char* mode = nullptr,
                    Stats* stats = nullptr, const std::atomic<bool>* cancel = nullptr);

  static const char* resultName(Result result);
};

}  // namespace VoiceRelay
