#include "SyncClient.h"

// clang-format off
#include <Arduino.h>
#include <Logging.h>
#include <NetBootstrap.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_random.h>
// clang-format on

#include <VoiceRelayCodec.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace Sync {
namespace {

constexpr size_t REQ_CAP = 128 * 1024;  // 50 ops, AI results with citations
constexpr size_t BODY_CAP = 16 * 1024;  // batch results
constexpr size_t URL_CAP = 160;
constexpr size_t AUTH_CAP = 192;
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 2048;
constexpr int HTTP_TIMEOUT_MS = 30000;
constexpr size_t ERROR_BODY_CAP = 255;

struct HeapCapsDeleter {
  void operator()(char* p) const { heap_caps_free(p); }
};
using Scratch = std::unique_ptr<char[], HeapCapsDeleter>;

Scratch allocScratch(const size_t size) {
  char* p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (p == nullptr) p = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return Scratch(p);
}

bool writeAll(esp_http_client_handle_t client, const char* data, const size_t len) {
  size_t sent = 0;
  while (sent < len) {
    const int n = esp_http_client_write(client, data + sent, static_cast<int>(len - sent));
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

Client::Result mapErrorBody(const int status, const char* body) {
  if (status == 401 || std::strstr(body, "UNAUTHORIZED") != nullptr ||
      std::strstr(body, "DEVICE_NOT_ALLOWED") != nullptr) {
    return Client::Result::Auth;
  }
  if (status >= 500) return Client::Result::Upstream;
  return Client::Result::Http;
}

}  // namespace

Client::Result Client::push(const char* url, const char* token, const char* deviceId, const char* const* ops,
                            const size_t count, PushResult* results, const size_t maxResults, bool& allAcked,
                            Stats* stats) {
  allAcked = false;
  if (stats != nullptr) *stats = Stats{};
  if (url == nullptr || token == nullptr || deviceId == nullptr || ops == nullptr || count == 0 || results == nullptr ||
      maxResults == 0) {
    return Result::BadConfig;
  }
  if (!WiFi.isConnected()) return Result::NoWifi;
  const VoiceRelay::ParsedUrl parsed = VoiceRelay::parseRelayUrl(url);
  if (!parsed.valid || token[0] == '\0' || deviceId[0] == '\0' || std::strlen(token) >= AUTH_CAP - 8) {
    return Result::BadConfig;
  }

  char httpUrl[URL_CAP];
  std::snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/sync/push", parsed.tls ? "https" : "http", parsed.host,
                static_cast<unsigned>(parsed.port));

  Scratch req = allocScratch(REQ_CAP);
  Scratch body = allocScratch(BODY_CAP);
  if (!req || !body) {
    LOG_ERR("SYNC", "OOM: request %u + body %u bytes", static_cast<unsigned>(REQ_CAP), static_cast<unsigned>(BODY_CAP));
    return Result::Oom;
  }

  char batchId[VoiceRelay::REQUEST_ID_CAP];
  VoiceRelay::formatRequestId(batchId, esp_random(), esp_random(), esp_random(), esp_random());
  const size_t reqLen = buildPushRequest(req.get(), REQ_CAP, deviceId, batchId, ops, count);
  if (reqLen == 0) {
    LOG_ERR("SYNC", "push request build failed (%u ops)", static_cast<unsigned>(count));
    return Result::BadConfig;
  }

  NetBootstrap::ensureTlsAllocator();
  if (parsed.tls && !NetBootstrap::ensureSystemTime()) return Result::Time;

  esp_http_client_config_t config = {};
  config.url = httpUrl;
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    LOG_ERR("SYNC", "client init failed");
    return Result::Connect;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  char auth[AUTH_CAP];
  std::snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);

  LOG_INF("SYNC", "POST %s (%u ops, %u body bytes)", httpUrl, static_cast<unsigned>(count),
          static_cast<unsigned>(reqLen));
  if (esp_http_client_open(client, static_cast<int64_t>(reqLen)) != ESP_OK) {
    LOG_ERR("SYNC", "open failed");
    esp_http_client_cleanup(client);
    return Result::Connect;
  }
  if (!writeAll(client, req.get(), reqLen)) {
    LOG_ERR("SYNC", "body write failed");
    esp_http_client_cleanup(client);
    return Result::Http;
  }

  esp_http_client_fetch_headers(client);
  const int status = esp_http_client_get_status_code(client);
  if (stats != nullptr) stats->httpStatus = status;

  if (status < 200 || status >= 300) {
    size_t n = 0;
    while (n < ERROR_BODY_CAP) {
      const int got = esp_http_client_read(client, body.get() + n, static_cast<int>(ERROR_BODY_CAP - n));
      if (got <= 0) break;
      n += static_cast<size_t>(got);
    }
    body[n] = '\0';
    esp_http_client_cleanup(client);
    LOG_ERR("SYNC", "HTTP %d: %s", status, n > 0 ? body.get() : "");
    return mapErrorBody(status, body.get());
  }

  size_t total = 0;
  while (total < BODY_CAP - 1) {
    const int got = esp_http_client_read(client, body.get() + total, static_cast<int>(BODY_CAP - 1 - total));
    if (got < 0) {
      esp_http_client_cleanup(client);
      LOG_ERR("SYNC", "read failed after %u bytes", static_cast<unsigned>(total));
      return Result::Read;
    }
    if (got == 0) break;
    total += static_cast<size_t>(got);
  }
  esp_http_client_cleanup(client);
  body[total] = '\0';
  if (stats != nullptr) stats->responseBytes = total;

  const size_t parsedCount = parsePushResponse(body.get(), total, results, maxResults, count, allAcked);
  if (parsedCount == 0) {
    LOG_ERR("SYNC", "bad push response (%u bytes)", static_cast<unsigned>(total));
    return Result::Protocol;
  }
  LOG_INF("SYNC", "pushed %u ops: %u results, allAcked=%d (http %d)", static_cast<unsigned>(count),
          static_cast<unsigned>(parsedCount), allAcked ? 1 : 0, status);
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
    case Result::Http:
      return "HTTP";
    case Result::Auth:
      return "AUTH";
    case Result::Upstream:
      return "UPSTREAM";
    case Result::Read:
      return "READ";
    case Result::Oom:
      return "OOM";
    case Result::Protocol:
      return "PROTOCOL";
  }
  return "?";
}

}  // namespace Sync
