#include "WeatherClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <CloudConfig.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <NetBootstrap.h>
#include <VoiceRelayCodec.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_task_wdt.h>

#include <cstdio>
#include <cstring>

namespace Weather {
namespace {

constexpr char CACHE_PATH[] = "/.crosspoint/weather.bin";
constexpr char CACHE_MAGIC[4] = {'W', 'X', 'R', '1'};

bool intLike(JsonVariantConst value) { return value.is<int>() || value.is<float>(); }

int clampTemp(int value) {
  if (value < -99) return -99;
  if (value > 99) return 99;
  return value;
}

}  // namespace

bool Client::begin() {
  body = makeUniqueNoThrow<char[]>(BODY_CAP);
  if (!body) {
    LOG_ERR("WX", "OOM: response buffer (%u bytes)", static_cast<unsigned>(BODY_CAP));
  }
  return body != nullptr;
}

Client::Result Client::fetch(Report& out) {
  if (!body) return Result::Memory;
  if (!WiFi.isConnected()) return Result::Wifi;

  struct Request {
    char url[320] = {};
    char auth[208] = {};
    esp_http_client_config_t config = {};
    VoiceRelay::ParsedUrl parsed;
  };
  auto req = makeUniqueNoThrow<Request>();
  if (!req) {
    LOG_ERR("WX", "OOM: request");
    return Result::Memory;
  }
  req->parsed = VoiceRelay::parseRelayUrl(CLOUD_CFG.relayUrl);
  if (!req->parsed.valid || !CLOUD_CFG.relayToken[0] || !CLOUD_CFG.relayDeviceId[0]) return Result::Config;

  const int n = snprintf(req->url, sizeof(req->url), "%s://%s:%u/api/v1/weather?days=%u",
                         req->parsed.tls ? "https" : "http", req->parsed.host, req->parsed.port, MAX_DAYS);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(req->url)) return Result::Config;

  NetBootstrap::ensureTlsAllocator();
  if (req->parsed.tls && !NetBootstrap::ensureSystemTime()) return Result::Unavailable;

  req->config.url = req->url;
  req->config.method = HTTP_METHOD_GET;
  req->config.buffer_size = 1024;
  req->config.buffer_size_tx = 512;
  req->config.timeout_ms = 18000;  // greater than the relay's upstream deadline
  req->config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t http = esp_http_client_init(&req->config);
  if (!http) {
    LOG_ERR("WX", "http init failed (heap=%u)", static_cast<unsigned>(ESP.getFreeHeap()));
    return Result::Memory;
  }
  snprintf(req->auth, sizeof(req->auth), "Bearer %s", CLOUD_CFG.relayToken);
  esp_http_client_set_header(http, "Authorization", req->auth);
  esp_http_client_set_header(http, "X-Device-Id", CLOUD_CFG.relayDeviceId);
  esp_http_client_set_header(http, "Accept", "application/json");

  if (esp_http_client_open(http, 0) != ESP_OK) {
    esp_http_client_cleanup(http);
    LOG_ERR("WX", "open failed (wifi=%d)", static_cast<int>(WiFi.status()));
    return Result::Unavailable;
  }
  esp_http_client_fetch_headers(http);
  const int status = esp_http_client_get_status_code(http);
  if (status != 200) {
    esp_http_client_cleanup(http);
    LOG_ERR("WX", "HTTP %d", status);
    if (status == 401 || status == 403) return Result::Auth;
    return Result::Unavailable;
  }

  size_t length = 0;
  const uint32_t start = millis();
  bool complete = false;
  while (length < BODY_CAP - 1 && millis() - start < 20000) {
    const int nread = esp_http_client_read(http, body.get() + length, BODY_CAP - 1 - length);
    if (nread < 0) break;
    length += nread;
    esp_task_wdt_reset();
    if (esp_http_client_is_complete_data_received(http)) {
      complete = true;
      break;
    }
    if (nread == 0) break;
  }
  esp_http_client_cleanup(http);
  if (!complete) {
    LOG_ERR("WX", "incomplete body (%u bytes)", static_cast<unsigned>(length));
    return Result::InvalidData;
  }
  body[length] = '\0';

  JsonDocument doc;
  if (deserializeJson(doc, body.get(), length) != DeserializationError::Ok) {
    LOG_ERR("WX", "invalid json (%u bytes)", static_cast<unsigned>(length));
    return Result::InvalidData;
  }

  Report report;
  const char* city = doc["city"] | "";
  snprintf(report.city, sizeof(report.city), "%s", city);
  report.stamp = doc["stamp"] | 0u;
  report.stale = doc["stale"] | false;
  JsonObjectConst current = doc["current"];
  if (!current.isNull()) {
    report.currentTemp = static_cast<int8_t>(clampTemp(current["t"] | 0));
    report.currentCategory = current["w"] | 0;
    if (report.currentCategory >= CATEGORY_COUNT) report.currentCategory = 2;
  }
  JsonArrayConst days = doc["days"];
  if (days.isNull() || days.size() == 0) return Result::InvalidData;
  for (JsonObjectConst day : days) {
    if (report.count >= MAX_DAYS) break;
    const char* date = day["d"] | "";
    if (strlen(date) < 10) continue;
    Day& entry = report.days[report.count];
    snprintf(entry.date, sizeof(entry.date), "%s", date);
    entry.hi = static_cast<int8_t>(clampTemp(day["hi"] | 0));
    entry.lo = static_cast<int8_t>(clampTemp(day["lo"] | 0));
    entry.rain = static_cast<uint8_t>(day["rain"] | 0);
    entry.category = day["w"] | 0;
    if (entry.category >= CATEGORY_COUNT) entry.category = 2;
    if (report.count == 0 && current.isNull()) {
      report.currentTemp = entry.hi;
      report.currentCategory = entry.category;
    }
    report.count++;
  }
  if (report.count == 0) return Result::InvalidData;
  report.valid = true;
  out = report;
  LOG_INF("WX", "%s %u days, current %d C, stale=%d", report.city, report.count, report.currentTemp,
          report.stale ? 1 : 0);
  return Result::Ok;
}

bool Client::loadCache(Report& out) {
  HalFile file;
  if (!Storage.openFileForRead("WX", CACHE_PATH, file)) return false;

  char magic[4] = {};
  if (file.read(magic, sizeof(magic)) != sizeof(magic) || memcmp(magic, CACHE_MAGIC, sizeof(magic)) != 0) {
    return false;  // different format version: treat as a miss
  }
  Report report;
  if (file.read(&report.stamp, sizeof(report.stamp)) != sizeof(report.stamp)) return false;
  if (file.read(&report.currentTemp, sizeof(report.currentTemp)) != sizeof(report.currentTemp)) return false;
  if (file.read(&report.currentCategory, sizeof(report.currentCategory)) != sizeof(report.currentCategory)) {
    return false;
  }
  if (file.read(&report.count, sizeof(report.count)) != sizeof(report.count)) return false;
  if (report.count > MAX_DAYS) return false;
  if (file.read(report.city, sizeof(report.city)) != sizeof(report.city)) return false;
  for (uint8_t i = 0; i < report.count; i++) {
    Day& day = report.days[i];
    if (file.read(day.date, sizeof(day.date)) != sizeof(day.date)) return false;
    if (file.read(&day.hi, sizeof(day.hi)) != sizeof(day.hi)) return false;
    if (file.read(&day.lo, sizeof(day.lo)) != sizeof(day.lo)) return false;
    if (file.read(&day.rain, sizeof(day.rain)) != sizeof(day.rain)) return false;
    if (file.read(&day.category, sizeof(day.category)) != sizeof(day.category)) return false;
  }
  report.valid = report.count > 0;
  out = report;
  return out.valid;
}

bool Client::saveCache(const Report& report) {
  Storage.mkdir("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("WX", CACHE_PATH, file)) return false;
  bool ok = file.write(CACHE_MAGIC, sizeof(CACHE_MAGIC)) == sizeof(CACHE_MAGIC);
  ok = ok && file.write(&report.stamp, sizeof(report.stamp)) == sizeof(report.stamp);
  ok = ok && file.write(&report.currentTemp, sizeof(report.currentTemp)) == sizeof(report.currentTemp);
  ok = ok && file.write(&report.currentCategory, sizeof(report.currentCategory)) == sizeof(report.currentCategory);
  ok = ok && file.write(&report.count, sizeof(report.count)) == sizeof(report.count);
  ok = ok && file.write(report.city, sizeof(report.city)) == sizeof(report.city);
  for (uint8_t i = 0; ok && i < report.count; i++) {
    const Day& day = report.days[i];
    ok = ok && file.write(day.date, sizeof(day.date)) == sizeof(day.date);
    ok = ok && file.write(&day.hi, sizeof(day.hi)) == sizeof(day.hi);
    ok = ok && file.write(&day.lo, sizeof(day.lo)) == sizeof(day.lo);
    ok = ok && file.write(&day.rain, sizeof(day.rain)) == sizeof(day.rain);
    ok = ok && file.write(&day.category, sizeof(day.category)) == sizeof(day.category);
  }
  return ok;
}

}  // namespace Weather
