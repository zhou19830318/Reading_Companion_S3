#include "BookLibraryClient.h"

// clang-format off
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <NetBootstrap.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_random.h>
#include <esp_task_wdt.h>
#include <freertos/task.h>
// clang-format on

#include <VoiceRelayCodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace BookLibrary {
namespace {

constexpr size_t BODY_CAP = 4096;
constexpr size_t URL_CAP = 256;
constexpr size_t AUTH_CAP = 192;
constexpr size_t UPLOAD_CHUNK = 16384;
constexpr int HTTP_TIMEOUT_MS = 45000;
constexpr uint32_t POLL_INTERVAL_MS = 1500;

struct HeapCapsDeleter {
  void operator()(char* p) const { heap_caps_free(p); }
};
using Scratch = std::unique_ptr<char[], HeapCapsDeleter>;

struct HttpBodySink {
  char* data;
  size_t capacity;
  size_t length;
  bool overflow = false;
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
    if (count < static_cast<size_t>(event->data_len)) sink->overflow = true;
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

void reportProgress(Client::ProgressCallback callback, void* ctx, bool uploading, uint8_t percent) {
  if (callback == nullptr) return;
  if (percent > 100) percent = 100;
  callback(ctx, Client::Progress{uploading, percent});
}

void delayCancellable(uint32_t totalMs, const std::atomic<bool>* cancel, uint32_t started, Client::Result& reason) {
  uint32_t waited = 0;
  while (waited < totalMs) {
    if (stopRequested(cancel, started, reason)) return;
    const uint32_t step = std::min<uint32_t>(100, totalMs - waited);
    vTaskDelay(pdMS_TO_TICKS(step));
    waited += step;
  }
}

Client::Result writeAll(esp_http_client_handle_t client, const char* data, size_t len, const std::atomic<bool>* cancel,
                        uint32_t started) {
  size_t sent = 0;
  while (sent < len) {
    Client::Result reason = Client::Result::Ok;
    if (stopRequested(cancel, started, reason)) return reason;
    const int n = esp_http_client_write(client, data + sent, static_cast<int>(len - sent));
    if (n <= 0) {
      if (stopRequested(cancel, started, reason)) return reason;
      return Client::Result::Http;
    }
    sent += static_cast<size_t>(n);
  }
  return Client::Result::Ok;
}

Client::Result mapHttpError(int status) {
  if (status == 401 || status == 403) return Client::Result::Auth;
  if (status == 404) return Client::Result::NotFound;
  if (status == 409) return Client::Result::Http;
  if (status == 413) return Client::Result::TooLarge;
  if (status == 415) return Client::Result::Unsupported;
  if (status >= 500) return Client::Result::Upstream;
  return Client::Result::Http;
}

Client::Result finishRequest(esp_http_client_handle_t client, Scratch& body, Client::Stats* stats, int& status) {
  if (esp_http_client_fetch_headers(client) < 0) return Client::Result::Connect;
  status = esp_http_client_get_status_code(client);
  if (stats != nullptr) stats->httpStatus = status;
  size_t total = 0;
  while (total < BODY_CAP - 1) {
    const int got = esp_http_client_read(client, body.get() + total, static_cast<int>(BODY_CAP - 1 - total));
    if (got == -ESP_ERR_HTTP_EAGAIN) return Client::Result::Timeout;
    if (got < 0) return Client::Result::Read;
    if (got == 0) break;
    total += static_cast<size_t>(got);
  }
  body[total] = '\0';
  if (stats != nullptr) stats->responseBytes = total;
  if (total == BODY_CAP - 1) return Client::Result::Read;
  if (status < 200 || status >= 300) {
    LOG_ERR("BOOK", "HTTP %d: %s", status, body.get());
    return mapHttpError(status);
  }
  return Client::Result::Ok;
}

void configureClient(esp_http_client_config_t& config, const char* httpUrl) {
  config.buffer_size = 2048;
  config.buffer_size_tx = 2048;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.url = httpUrl;
}

void setAuth(esp_http_client_handle_t client, const char* token, const char* deviceId) {
  char auth[AUTH_CAP];
  std::snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);
  esp_http_client_set_header(client, "X-Device-Id", deviceId);
  esp_http_client_set_header(client, "Accept", "application/json");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
}

Client::Result uploadBook(const char* url, const char* token, const char* deviceId, const char* bookPath,
                          Client::ProgressCallback onProgress, void* ctx, IngestResponse& response,
                          Client::Stats* stats, const std::atomic<bool>* cancel, uint32_t started) {
  HalFile file;
  if (!Storage.openFileForRead("BOOK", bookPath, file) || !file) return Client::Result::Read;
  const size_t fileSize = file.size();
  if (fileSize == 0) return Client::Result::Read;

  const char* fileName = std::strrchr(bookPath, '/');
  fileName = fileName != nullptr ? fileName + 1 : bookPath;
  char safeName[160];
  std::snprintf(safeName, sizeof(safeName), "%s", fileName);
  for (char* p = safeName; *p != '\0'; ++p) {
    if (*p == '"' || *p == '\r' || *p == '\n') *p = '_';
  }

  char boundary[40];
  std::snprintf(boundary, sizeof(boundary), "----CrossPoint%08x%08x", esp_random(), esp_random());
  char header[512];
  const int headerLen = std::snprintf(header, sizeof(header),
                                      "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                                      "Content-Type: application/epub+zip\r\n\r\n",
                                      boundary, safeName);
  constexpr char footer[] = "\r\n--";
  char footerTail[48];
  const int footerLen = std::snprintf(footerTail, sizeof(footerTail), "%s--\r\n", boundary);
  if (headerLen <= 0 || static_cast<size_t>(headerLen) >= sizeof(header) || footerLen <= 0 ||
      static_cast<size_t>(footerLen) >= sizeof(footerTail)) {
    return Client::Result::BadConfig;
  }

  const VoiceRelay::ParsedUrl parsed = VoiceRelay::parseRelayUrl(url);
  char httpUrl[URL_CAP];
  std::snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/books", parsed.tls ? "https" : "http", parsed.host,
                static_cast<unsigned>(parsed.port));

  Scratch body = allocScratch(BODY_CAP);
  Scratch buffer = allocScratch(UPLOAD_CHUNK);
  if (!body || !buffer) return Client::Result::Oom;

  NetBootstrap::ensureTlsAllocator();
  if (parsed.tls && !NetBootstrap::ensureSystemTime()) return Client::Result::Time;

  esp_http_client_config_t config = {};
  configureClient(config, httpUrl);
  config.method = HTTP_METHOD_POST;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return Client::Result::Connect;
  char contentType[80];
  std::snprintf(contentType, sizeof(contentType), "multipart/form-data; boundary=%s", boundary);
  esp_http_client_set_header(client, "Content-Type", contentType);
  setAuth(client, token, deviceId);

  const uint64_t contentLength = static_cast<uint64_t>(headerLen) + fileSize + sizeof(footer) - 1 + footerLen;
  LOG_INF("BOOK", "upload %s (%u bytes)", fileName, static_cast<unsigned>(fileSize));
  reportProgress(onProgress, ctx, true, 0);
  if (esp_http_client_open(client, static_cast<int64_t>(contentLength)) != ESP_OK) {
    esp_http_client_cleanup(client);
    return Client::Result::Connect;
  }

  Client::Result result = writeAll(client, header, static_cast<size_t>(headerLen), cancel, started);
  size_t uploaded = 0;
  uint8_t lastPercent = 0;
  while (result == Client::Result::Ok && uploaded < fileSize) {
    const size_t want = std::min(UPLOAD_CHUNK, fileSize - uploaded);
    const int got = file.read(buffer.get(), want);
    if (got <= 0) {
      result = Client::Result::Read;
      break;
    }
    result = writeAll(client, buffer.get(), static_cast<size_t>(got), cancel, started);
    if (result != Client::Result::Ok) break;
    uploaded += static_cast<size_t>(got);
    const auto percent = static_cast<uint8_t>(static_cast<uint64_t>(uploaded) * 100 / fileSize);
    if (percent >= lastPercent + 10 || percent == 100) {
      lastPercent = percent;
      reportProgress(onProgress, ctx, true, percent);
    }
    if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
  }
  if (result == Client::Result::Ok) {
    result = writeAll(client, footer, sizeof(footer) - 1, cancel, started);
  }
  if (result == Client::Result::Ok) {
    result = writeAll(client, footerTail, static_cast<size_t>(footerLen), cancel, started);
  }

  int status = 0;
  if (result == Client::Result::Ok) result = finishRequest(client, body, stats, status);
  esp_http_client_cleanup(client);
  if (result != Client::Result::Ok) return result;
  if (!parseIngestResponse(body.get(), stats != nullptr ? stats->responseBytes : std::strlen(body.get()), nullptr,
                           response)) {
    return Client::Result::Parse;
  }
  if (response.status == Status::Failed) return Client::Result::IngestFailed;
  return Client::Result::Ok;
}

Client::Result getProcessing(const char* url, const char* token, const char* deviceId, const char* bookId,
                             Client::ProgressCallback onProgress, void* ctx, IngestResponse& response,
                             Client::Stats* stats, const std::atomic<bool>* cancel, uint32_t started) {
  const VoiceRelay::ParsedUrl parsed = VoiceRelay::parseRelayUrl(url);
  char httpUrl[URL_CAP];
  std::snprintf(httpUrl, sizeof(httpUrl), "%s://%s:%u/api/v1/books/%s/processing", parsed.tls ? "https" : "http",
                parsed.host, static_cast<unsigned>(parsed.port), bookId);
  Scratch body = allocScratch(BODY_CAP);
  if (!body) return Client::Result::Oom;

  NetBootstrap::ensureTlsAllocator();
  if (parsed.tls && !NetBootstrap::ensureSystemTime()) return Client::Result::Time;

  HttpBodySink sink{body.get(), BODY_CAP - 1, 0};
  esp_http_client_config_t config = {};
  configureClient(config, httpUrl);
  config.method = HTTP_METHOD_GET;
  config.event_handler = httpBodyEvent;
  config.user_data = &sink;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return Client::Result::Connect;
  setAuth(client, token, deviceId);
  const esp_err_t performResult = esp_http_client_perform(client);
  Client::Result reason = Client::Result::Ok;
  if (stopRequested(cancel, started, reason)) {
    esp_http_client_cleanup(client);
    return reason;
  }
  if (performResult != ESP_OK) {
    esp_http_client_cleanup(client);
    return Client::Result::Connect;
  }
  const int status = esp_http_client_get_status_code(client);
  const size_t total = sink.length;
  if (stats != nullptr) {
    stats->httpStatus = status;
    stats->responseBytes = total;
  }
  esp_http_client_cleanup(client);
  if (sink.overflow || total == BODY_CAP - 1) return Client::Result::Read;
  body[total] = '\0';
  if (status < 200 || status >= 300) {
    LOG_ERR("BOOK", "processing HTTP %d: %s", status, body.get());
    return mapHttpError(status);
  }
  if (!parseIngestResponse(body.get(), total, bookId, response)) return Client::Result::Parse;
  reportProgress(onProgress, ctx, false, static_cast<uint8_t>(response.progress / 5 * 5));
  if (response.status == Status::Ready) return Client::Result::Ok;
  if (response.status == Status::Failed) return Client::Result::IngestFailed;
  return Client::Result::Http;
}

Client::Result waitUntilReady(const char* url, const char* token, const char* deviceId, const char* bookId,
                              Client::ProgressCallback onProgress, void* ctx, IngestResponse& response,
                              Client::Stats* stats, const std::atomic<bool>* cancel, uint32_t started) {
  bool first = true;
  while (true) {
    Client::Result reason = Client::Result::Ok;
    if (stopRequested(cancel, started, reason)) return reason;
    if (!first) delayCancellable(POLL_INTERVAL_MS, cancel, started, reason);
    if (reason != Client::Result::Ok) return reason;
    first = false;
    reason = getProcessing(url, token, deviceId, bookId, onProgress, ctx, response, stats, cancel, started);
    if (reason == Client::Result::Ok) return reason;
    if (reason != Client::Result::Http) return reason;
  }
}

}  // namespace

Client::Result Client::ensureReady(const char* url, const char* token, const char* deviceId, const char* bookId,
                                   const char* bookPath, const bool upload, ProgressCallback onProgress, void* ctx,
                                   IngestResponse& response, Stats* stats, const std::atomic<bool>* cancel) {
  const uint32_t started = millis();
  Result reason = Result::Ok;
  if (stats != nullptr) *stats = Stats{};
  response = IngestResponse{};
  if (url == nullptr || token == nullptr || deviceId == nullptr || bookId == nullptr || bookId[0] == '\0' ||
      (upload && (bookPath == nullptr || bookPath[0] == '\0'))) {
    return Result::BadConfig;
  }
  if (stopRequested(cancel, started, reason)) return reason;
  if (!WiFi.isConnected()) return Result::NoWifi;
  const VoiceRelay::ParsedUrl parsed = VoiceRelay::parseRelayUrl(url);
  if (!parsed.valid || token[0] == '\0' || deviceId[0] == '\0' || std::strlen(token) >= AUTH_CAP - 8) {
    return Result::BadConfig;
  }

  if (upload) {
    reason = uploadBook(url, token, deviceId, bookPath, onProgress, ctx, response, stats, cancel, started);
    if (reason != Result::Ok) return reason;
    if (std::strcmp(response.bookId, bookId) != 0) return Result::Parse;
    reportProgress(onProgress, ctx, false, static_cast<uint8_t>(response.progress));
    if (response.status == Status::Failed) return Result::IngestFailed;
    if (response.status == Status::Ready) return Result::Ok;
  } else {
    std::snprintf(response.bookId, sizeof(response.bookId), "%s", bookId);
  }
  return waitUntilReady(url, token, deviceId, bookId, onProgress, ctx, response, stats, cancel, started);
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
    case Result::TooLarge:
      return "TOO_LARGE";
    case Result::Unsupported:
      return "UNSUPPORTED";
    case Result::Upstream:
      return "UPSTREAM";
    case Result::IngestFailed:
      return "INGEST_FAILED";
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

}  // namespace BookLibrary
