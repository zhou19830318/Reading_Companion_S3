#include "TtsPlayer.h"

// clang-format off
// Arduino.h first (WebSockets pulls lwip, which needs the Arduino headers
// visible before it).
#include <Arduino.h>
// clang-format on
#include <CloudConfig.h>
#include <HalAudio.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <Memory.h>
#include <VoiceRelayCodec.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

namespace VoiceRelay {
namespace {
// Device-side budgets. The relay synthesises in ~60-character segments and its
// first frame lands 1.3–2.1 s after tts.start; 20 s covers a cold edge-tts dial.
constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
constexpr uint32_t META_TIMEOUT_MS = 20000;
// Must outlast the relay's per-segment synthesis budget (TTS_TIMEOUT_S=15 s):
// a cold edge-tts dial or a throttled provider round can leave the first frame
// pending well past the old 6 s, and aborting there killed read-aloud sessions
// with "stream stalled after 0 frames". Real failures still surface immediately
// as tts.error text, so a longer safety net only delays the wedged-server case.
constexpr uint32_t FRAME_GAP_TIMEOUT_MS = 20000;
// Hard ceiling so a wedged server cannot hold the caller (and the amp) forever.
// A read-aloud page can be ~1200 Chinese characters (~4-5 minutes of audio),
// so the old 2-minute cap truncated the second half of long pages.
constexpr uint32_t SESSION_CAP_MS = 600000;
// Playback volume comes from the app's volume setting via setVolumeSource()
// (the lib layer cannot see CrossPointSettings). edge-tts PCM is near full
// scale and the MAX98357A gain is strapped, so the PCM trim here is the only
// loudness control; DEFAULT_GAIN_PERCENT (header) is the no-source fallback.
// tts.start is built once; the relay truncates at TTS_MAX_CHARS=1200.
constexpr size_t TTS_TEXT_CAP = 1200;
constexpr size_t TTS_JSON_CAP = TTS_TEXT_CAP * 2 + 128;

// The ESP-IDF certificate bundle is already linked (ssl_client.cpp references
// it), so naming it here costs no flash and keeps wss:// dials on chain
// validation instead of setInsecure(). Same source as VoiceRelayClient.cpp.
extern "C" {
extern const uint8_t _binary_x509_crt_bundle_start[];
extern const uint8_t _binary_x509_crt_bundle_end[];
}

const uint8_t* tlsBundle() { return _binary_x509_crt_bundle_start; }
size_t tlsBundleSize() { return static_cast<size_t>(_binary_x509_crt_bundle_end - _binary_x509_crt_bundle_start); }

// JSON string escaping for the one field we send. The relay type-checks text,
// so control characters are folded to spaces rather than escaped ("\u0000"
// is still a bad_text round trip).
size_t escapeInto(char* out, size_t cap, const char* in) {
  size_t used = 0;
  for (const char* p = in; *p != '\0' && used + 1 < cap; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      if (used + 2 >= cap) break;
      out[used++] = '\\';
      out[used++] = static_cast<char>(c);
    } else if (c < 0x20) {
      out[used++] = ' ';
    } else {
      out[used++] = static_cast<char>(c);
    }
  }
  out[used] = '\0';
  return used;
}
}  // namespace

uint8_t (*TtsPlayer::volumeSource_)() = nullptr;

void TtsPlayer::setVolumeSource(uint8_t (*getter)()) { volumeSource_ = getter; }

uint8_t TtsPlayer::gainPercent() {
  const uint8_t v = volumeSource_ ? volumeSource_() : DEFAULT_GAIN_PERCENT;
  return v > 100 ? 100 : v;
}

const char* TtsPlayer::resultName(Result result) {
  switch (result) {
    case Result::Ok:
      return "ok";
    case Result::Config:
      return "config";
    case Result::Wifi:
      return "wifi";
    case Result::Auth:
      return "auth";
    case Result::Unavailable:
      return "unavailable";
    case Result::Memory:
      return "memory";
  }
  return "?";
}

void TtsPlayer::stop() { stopRequested_.store(true, std::memory_order_relaxed); }

TtsPlayer::Result TtsPlayer::speak(const char* text, const char* lang) {
  if (text == nullptr || text[0] == '\0') return Result::Config;
  // A new stream is a new request: a stop aimed at the previous one must not
  // abort this one before it starts.
  stopRequested_.store(false, std::memory_order_relaxed);
  // NVS, idempotent: the same source every other relay consumer reads, so a
  // fresh boot (or the serial debug path) does not need the settings screen to
  // populate the singleton first.
  CLOUD_CFG.load();
  const ParsedUrl parsed = parseRelayUrl(CLOUD_CFG.relayUrl);
  if (!parsed.valid || CLOUD_CFG.relayToken[0] == '\0' || CLOUD_CFG.relayDeviceId[0] == '\0') {
    LOG_ERR("TTS", "relay not configured");
    return Result::Config;
  }
  if (!WiFi.isConnected()) return Result::Wifi;
  if (!AUDIO.begin()) {
    LOG_ERR("TTS", "audio unavailable");
    return Result::Unavailable;
  }

  // 240 MHz for the whole stream: i2s_channel_enable() permanently blocks when
  // the CPU is in low-power mode (see the S3 integration notes, pitfall #1).
  HalPowerManager::Lock powerLock;

  auto json = makeUniqueNoThrow<char[]>(TTS_JSON_CAP);
  if (!json) {
    LOG_ERR("TTS", "OOM: %u bytes", static_cast<unsigned>(TTS_JSON_CAP));
    return Result::Memory;
  }
  char body[TTS_TEXT_CAP];
  escapeInto(body, sizeof(body), text);
  const char* language = (lang != nullptr && lang[0] != '\0') ? lang : "zh";
  const int len = snprintf(json.get(), TTS_JSON_CAP,
                           "{\"type\":\"tts.start\",\"id\":1,\"text\":\"%s\",\"lang\":\"%s\"}", body, language);
  if (len <= 0 || static_cast<size_t>(len) >= TTS_JSON_CAP) {
    LOG_ERR("TTS", "tts.start too long (%d)", len);
    return Result::Config;
  }

  release();
  ws_ = makeUniqueNoThrow<WebSocketsClient>();
  if (!ws_) {
    LOG_ERR("TTS", "OOM: WebSocketsClient");
    return Result::Memory;
  }

  char auth[sizeof(CLOUD_CFG.relayToken) + 8];
  snprintf(auth, sizeof(auth), "Bearer %s", CLOUD_CFG.relayToken);
  char headers[sizeof(CLOUD_CFG.relayDeviceId) + 24];
  snprintf(headers, sizeof(headers), "X-Device-Id: %s", CLOUD_CFG.relayDeviceId);

  ws_->onEvent([this](WStype_t type, uint8_t* payload, size_t length) { onEvent(type, payload, length); });
  // No reconnect interval: a stream is one request/response pair and speak()
  // releases the socket afterwards. A long interval would also gate the FIRST
  // dial (WebSocketsClient starts _lastConnectionFail at 0, so millis() must
  // exceed the interval before the initial connect), killing any TTS round
  // started in the first minute after boot.
  if (parsed.tls) {
    ws_->beginSslWithBundle(parsed.host, parsed.port, "/api/v1/tts", tlsBundle(), tlsBundleSize(), "");
  } else {
    ws_->begin(parsed.host, parsed.port, "/api/v1/tts", "");
  }
  // After begin(): it zeroes plainAuthorization, so a Bearer set earlier is
  // silently dropped and the upgrade comes back UNAUTHORIZED.
  ws_->setAuthorization(auth);
  ws_->setExtraHeaders(headers);

  startedAtMs_ = millis();
  lastDataMs_ = startedAtMs_;
  LOG_INF("TTS", "dialing %s://%s:%u/api/v1/tts (%u chars, lang=%s)", parsed.tls ? "wss" : "ws", parsed.host,
          static_cast<unsigned>(parsed.port), static_cast<unsigned>(strlen(text)), language);

  bool startSent = false;
  while (!ended_ && !failed_) {
    if (stopRequested_.load(std::memory_order_relaxed)) {
      LOG_INF("TTS", "stopped by caller");
      snprintf(errorCode_, sizeof(errorCode_), "stopped");
      failed_ = true;
      break;
    }
    ws_->loop();
    if (!startSent && opened_) {
      startSent = ws_->sendTXT(json.get());
      if (!startSent) {
        LOG_ERR("TTS", "tts.start send failed");
        failed_ = true;
        break;
      }
      lastDataMs_ = millis();
    }

    const uint32_t now = millis();
    const uint32_t elapsed = now - startedAtMs_;
    if (!opened_ && elapsed > CONNECT_TIMEOUT_MS) {
      LOG_ERR("TTS", "connect timeout");
      failed_ = true;
    } else if (startSent && !gotMeta_ && now - lastDataMs_ > META_TIMEOUT_MS) {
      LOG_ERR("TTS", "no tts.meta");
      failed_ = true;
    } else if (gotMeta_ && !ended_ && now - lastDataMs_ > FRAME_GAP_TIMEOUT_MS) {
      LOG_ERR("TTS", "stream stalled after %u frames", static_cast<unsigned>(frames_));
      failed_ = true;
    } else if (elapsed > SESSION_CAP_MS) {
      LOG_ERR("TTS", "session cap hit");
      failed_ = true;
    }
    // The main task is not a WDT subscriber; feeding it here only logged
    // "task not found". Loop progress is bounded by the timers above instead.
    delay(1);
  }

  // Snapshot before release(): it clears the per-stream counters, and the
  // summary below is the only evidence a caller has that audio left the
  // speaker.
  const bool hadMeta = gotMeta_;
  const bool hadAudio = gotAudio_;
  const uint32_t frames = frames_;
  const uint32_t bytes = bytesPlayed_;
  char code[sizeof(errorCode_)];
  snprintf(code, sizeof(code), "%s", errorCode_);

  ampOff();
  AUDIO.end();
  release();

  if (failed_) {
    LOG_ERR("TTS", "failed: %s", code[0] ? code : "transport");
    if (strcmp(code, "auth") == 0) return Result::Auth;
    return Result::Unavailable;
  }
  if (!hadAudio) {
    LOG_ERR("TTS", "no audio (meta=%d frames=%u)", hadMeta ? 1 : 0, static_cast<unsigned>(frames));
    return Result::Unavailable;
  }
  LOG_INF("TTS", "done: %u frames, %u bytes, %.1f s audio, %lu ms wall", static_cast<unsigned>(frames),
          static_cast<unsigned>(bytes), bytes / 32000.0f, static_cast<unsigned long>(millis() - startedAtMs_));
  return Result::Ok;
}

void TtsPlayer::onEvent(int type, uint8_t* payload, size_t length) {
  switch (static_cast<WStype_t>(type)) {
    case WStype_CONNECTED:
      opened_ = true;
      LOG_DBG("TTS", "socket open");
      break;
    case WStype_DISCONNECTED:
      if (!ended_) {
        LOG_ERR("TTS", "socket closed (meta=%d frames=%u)", gotMeta_ ? 1 : 0, static_cast<unsigned>(frames_));
        failed_ = true;
      }
      break;
    case WStype_TEXT:
      if (payload != nullptr && length > 0) {
        LOG_DBG("TTS", "text[%u]: %.*s", static_cast<unsigned>(length), static_cast<int>(length > 200 ? 200 : length),
                reinterpret_cast<const char*>(payload));
        handleText(reinterpret_cast<const char*>(payload), length);
      }
      break;
    case WStype_BIN:
      if (!gotMeta_) {
        // Audio before the header would mean the relay changed shape; playing
        // it blind risks I2S at the wrong rate, so fail loudly instead.
        LOG_ERR("TTS", "binary frame before tts.meta");
        failed_ = true;
        break;
      }
      if (payload != nullptr && length > 0) {
        playPcm(payload, length);
        lastDataMs_ = millis();
      }
      break;
    case WStype_ERROR:
      LOG_ERR("TTS", "websocket error");
      failed_ = true;
      break;
    default:
      LOG_DBG("TTS", "event type=%d len=%u", type, static_cast<unsigned>(length));
      break;
  }
}

void TtsPlayer::handleText(const char* json, size_t length) {
  // Field extraction without a JSON parser: the relay sends at most four small
  // objects on this socket, and pulling in ArduinoJson here would cost flash
  // for four strstr calls.
  (void)length;
  if (strstr(json, "\"tts.meta\"") != nullptr) {
    gotMeta_ = true;
    ampOn();
    LOG_DBG("TTS", "meta: %.*s", static_cast<int>(length > 160 ? 160 : length), json);
    return;
  }
  if (strstr(json, "\"tts.end\"") != nullptr) {
    ended_ = true;
    LOG_DBG("TTS", "end: %.*s", static_cast<int>(length > 120 ? 120 : length), json);
    return;
  }
  if (strstr(json, "\"tts.error\"") != nullptr) {
    const char* code = strstr(json, "\"code\":\"");
    if (code != nullptr) {
      code += 8;
      const char* end = strchr(code, '"');
      const size_t n = (end != nullptr) ? static_cast<size_t>(end - code) : 0;
      if (n > 0 && n < sizeof(errorCode_)) {
        memcpy(errorCode_, code, n);
        errorCode_[n] = '\0';
      }
    }
    failed_ = true;
    LOG_ERR("TTS", "relay error: %s", errorCode_[0] ? errorCode_ : json);
  }
}

void TtsPlayer::playPcm(const uint8_t* data, size_t length) {
  const int gain = gainPercent();
  size_t offset = 0;
  // A frame must arrive on a 2-byte boundary for int16_t; hold a trailing odd
  // byte until the next frame instead of dropping it (a dropped byte shifts
  // every later sample by one and turns speech into noise).
  if (hasCarry_) {
    const uint16_t lo = carry_;
    const uint16_t hi = data[0];
    pcm_[0] = static_cast<int16_t>((lo | (hi << 8)) * gain / 100);
    hasCarry_ = false;
    offset = 1;
    bytesPlayed_ += 2;
    if (!AUDIO.writePcm(pcm_, 1)) {
      LOG_ERR("TTS", "audio write failed");
      failed_ = true;
      return;
    }
  }
  while (offset + 1 < length) {
    size_t samples = (length - offset) / 2;
    if (samples > sizeof(pcm_) / sizeof(pcm_[0])) samples = sizeof(pcm_) / sizeof(pcm_[0]);
    memcpy(pcm_, data + offset, samples * 2);
    for (size_t i = 0; i < samples; ++i) {
      pcm_[i] = static_cast<int16_t>(pcm_[i] * gain / 100);
    }
    if (!AUDIO.writePcm(pcm_, samples)) {
      LOG_ERR("TTS", "audio write failed at %u", static_cast<unsigned>(bytesPlayed_));
      failed_ = true;
      return;
    }
    offset += samples * 2;
    bytesPlayed_ += samples * 2;
  }
  if (offset < length) {
    carry_ = data[offset];
    hasCarry_ = true;
  }
  gotAudio_ = true;
  frames_++;
}

void TtsPlayer::ampOn() {
  AUDIO.setEnabled(true);
  delay(5);  // MAX98357A ground-switch / bias settle, same as UiSound
}

void TtsPlayer::ampOff() {
  if (!AUDIO.isEnabled()) return;
  delay(300);  // let the last (up to 256 ms) DMA buffer reach the amplifier
  AUDIO.setEnabled(false);
}

void TtsPlayer::shutdown() {
  release();
  // Normally speak() already did this; the path here is activity teardown
  // after a force-deleted task, where the amp must not stay on.
  ampOff();
  AUDIO.end();
}

void TtsPlayer::release() {
  if (ws_) {
    ws_->disconnect();
    ws_.reset();
  }
  opened_ = false;
  gotMeta_ = false;
  gotAudio_ = false;
  ended_ = false;
  failed_ = false;
  hasCarry_ = false;
  carry_ = 0;
  errorCode_[0] = '\0';
  bytesPlayed_ = 0;
  frames_ = 0;
}

}  // namespace VoiceRelay
