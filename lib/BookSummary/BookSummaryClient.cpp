#include "BookSummaryClient.h"

// clang-format off
#include <Arduino.h>
#include <Logging.h>
#include <NetBootstrap.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
// clang-format on

#include <VoiceRelayCodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace BookSummary {
namespace {

constexpr size_t BODY_CAP = 16384;
constexpr size_t URL_CAP = 256;
constexpr size_t AUTH_CAP = 192;
constexpr int HTTP_TIMEOUT_MS = 45000;

struct HeapCapsDeleter {
  void operator()(char* p) const { heap_caps_free(p); }
};
using Scratch = std::unique_ptr<char[], HeapCapsDeleter>;

struct HttpBodySink {
  char* data;
  size_t capacity;
  size_t length;
};

esp_err_t httpBodyEvent(esp_http_client_event_t* event) {
  auto* sink = static_cast<HttpBodySink*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
    const size_t available = sink->capacity > sink->length ? sink->capacity - sink->length : 0;
    const size_t count = std::min(available, static_cast<size_t>(event->data_len));
    if (count > 0) {
      std::memcpy(sink->data + sink->length, event->data, count);
      sink->length += count;
    }
  } else if (event->event_id == HTTP_EVENT_ERROR) {
    return ESP_FAIL;
  }
  return ESP_OK;
}

Scratch allocScratch(size_t size) {
  char* p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (p == nullptr) p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return Scratch(p);
}

bool stopRequested(const std::atomic<bool>* cancel, uint32_t started, Client::Result& reason) {
  if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
    reason = Client::Result::Cancelled;
    return true;
  }
  if (static_cast<uint32_t>(millis() - started) >= Client::TOTAL_TIMEOUT_MS) {
    reason = Client::Result::Timeout;
    return true;
  }
  return false;
}

Client::Result mapStatus(int status) {
  if (status == 401) return Client::Result::Auth;
  if (status == 404) return Client::Result::NotFound;
  if (status == 409) return Client::Result::NotReady;
  if (status >= 500) return Client::Result::Upstream;
  return Client::Result::Http;
}

}  // namespace

Client::Result Client::get(const char* url, const char* token, const char* deviceId, const char* bookId,
                           const bool wholeBook, const int spineIndex, Response& response, Stats* stats,
                           const std::atomic<bool>* cancel) {
  const uint32_t started = millis();
  Result reason = Result::Ok;
  if (stats != nullptr) *stats = Stats{};
  if (url == nullptr || token == nullptr || deviceId == nullptr || bookId == nullptr || bookId[0] == '\0') {
    return Result::BadConfig;
  }
  if (stopRequested(cancel, started, reason)) return reason;
  if (!WiFi.isConnected()) return Result::NoWifi;
  const VoiceRelay::ParsedUrl parsed = VoiceRelay::parseRelayUrl(url);
  if (!parsed.valid || token[0] == '\0' || deviceId[0] == '\0' || std::strlen(token) >= AUTH_CAP - 8) {
    return Result::BadConfig;
  }

  char httpUrl[URL_CAP];
  if (wholeBook) {
    std::snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/books/%s/summary?scope=book",
                  parsed.tls ? "https" : "http", parsed.host, static_cast<unsigned>(parsed.port), bookId);
  } else {
    std::snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/books/%s/summary?scope=chapter&spine_index=%d",
                  parsed.tls ? "https" : "http", parsed.host, static_cast<unsigned>(parsed.port), bookId, spineIndex);
  }

  Scratch body = allocScratch(BODY_CAP);
  if (!body) return Result::Oom;

  NetBootstrap::ensureTlsAllocator();
  if (parsed.tls && !NetBootstrap::ensureSystemTime()) return Result::Time;

  HttpBodySink sink{body.get(), BODY_CAP - 1, 0};
  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = HTTP_METHOD_GET;
  config.buffer_size = 2048;
  config.buffer_size_tx = 2048;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = httpBodyEvent;
  config.user_data = &sink;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return Result::Connect;
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  char auth[AUTH_CAP];
  std::snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);

  LOG_INF("SUMMARY", "GET %s", httpUrl);
  const esp_err_t performResult = esp_http_client_perform(client);
  LOG_INF("SUMMARY", "perform result: %d", static_cast<int>(performResult));
  if (stopRequested(cancel, started, reason)) {
    esp_http_client_cleanup(client);
    return reason;
  }
  if (performResult != ESP_OK) {
    esp_http_client_cleanup(client);
    return Result::Connect;
  }
  const int status = esp_http_client_get_status_code(client);
  const size_t total = sink.length;
  if (stats != nullptr) stats->httpStatus = status;
  LOG_INF("SUMMARY", "headers: http %d, body %u", status, static_cast<unsigned>(total));
  esp_http_client_cleanup(client);
  LOG_INF("SUMMARY", "client cleaned up");
  body[total] = '\0';
  if (stats != nullptr) stats->responseBytes = total;

  if (status < 200 || status >= 300) {
    LOG_ERR("SUMMARY", "HTTP %d: %s", status, body.get());
    return mapStatus(status);
  }
  if (total == BODY_CAP - 1) return Result::Read;
  LOG_INF("SUMMARY", "parsing response");
  const bool parseOk = parseResponse(body.get(), total, bookId, response);
  LOG_INF("SUMMARY", "parse result: %d", parseOk ? 1 : 0);
  if (!parseOk) return Result::Parse;
  LOG_INF("SUMMARY", "ok: %u bytes (http %d, cached %d)", static_cast<unsigned>(total), status, response.cached);
  return Result::Ok;
}

const char* Client::resultName(const Result result) {
  switch (result) {
    case Result::Ok:
      return "OK";
    case Result::BadConfig:
      return "BAD_CONFIG";
    case Result::NoWifi:
      return "NO_WIFI";
    case Result::Time:
      return "TIME";
    case Result::Connect:
      return "CONNECT";
    case Result::Timeout:
      return "TIMEOUT";
    case Result::Http:
      return "HTTP";
    case Result::Auth:
      return "AUTH";
    case Result::NotFound:
      return "NOT_FOUND";
    case Result::NotReady:
      return "NOT_READY";
    case Result::Upstream:
      return "UPSTREAM";
    case Result::Read:
      return "READ";
    case Result::Oom:
      return "OOM";
    case Result::Parse:
      return "PARSE";
    case Result::Cancelled:
      return "CANCELLED";
  }
  return "?";
}

}  // namespace BookSummary
