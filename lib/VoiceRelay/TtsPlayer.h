#pragma once

#include <WebSocketsClient.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

// Text-to-speech playback over the relay's /api/v1/tts socket.
//
// Protocol (relay side, already deployed — docs/s3-peripherals-integration-plan.md §6.2):
//   device -> {"type":"tts.start","id":N,"text":"…","lang":"zh"}
//   relay  -> {"type":"tts.meta","rate":16000,"ch":1,"bits":16,"voice":"…"}
//   relay  -> <binary s16le, 16 kHz mono, 8 KB frames>
//   relay  -> {"type":"tts.end","ms":N} | {"type":"tts.error","code":"…"}
//
// Task model: speak() owns the socket for its whole lifetime and drives
// ws->loop() itself, so every callback runs on the caller's task and no
// locking is needed. Nothing is sent from inside a callback.
//
// Buffering: decoded PCM is pushed straight into the I2S DMA ring from the
// callback. The DMA ring (256 ms) is the only buffer, which keeps the player
// under 1 KB of DRAM and turns TCP backpressure into flow control instead of
// a second queue. A block-length hiccup therefore shows up as a gap only when
// it exceeds one whole relay frame.
namespace VoiceRelay {

class TtsPlayer {
 public:
  enum class Result { Ok, Config, Wifi, Auth, Unavailable, Memory };

  TtsPlayer() = default;
  ~TtsPlayer() { shutdown(); }
  TtsPlayer(const TtsPlayer&) = delete;
  TtsPlayer& operator=(const TtsPlayer&) = delete;

  // Blocking: dials, synthesises and plays `text`, returning when the stream
  // ends (Ok) or on the first failure. `lang` is "zh" or "en"; empty defaults
  // to "zh". Caller is expected to have Wi-Fi up (see Result::Wifi).
  Result speak(const char* text, const char* lang);

  // Ask a running speak() to return at the next loop pass. Safe from another
  // task; the socket is still closed by speak() on its own task.
  void stop();

  static const char* resultName(Result result);

  // Register the source of the playback volume (0-100). The lib layer cannot
  // see CrossPointSettings, so the app supplies a getter once at boot; every
  // TtsPlayer reads the current value through it. Without a source the player
  // falls back to DEFAULT_GAIN_PERCENT. The getter may run on the TTS worker
  // task, so it must be a cheap, thread-safe read (a settings byte is).
  static void setVolumeSource(uint8_t (*getter)());

 private:
  static constexpr uint8_t DEFAULT_GAIN_PERCENT = 25;
  static uint8_t gainPercent();
  static uint8_t (*volumeSource_)();
  void onEvent(int type, uint8_t* payload, size_t length);
  void handleText(const char* json, size_t length);
  void playPcm(const uint8_t* data, size_t length);
  void ampOn();
  void ampOff();
  void release();
  void shutdown();

  std::unique_ptr<WebSocketsClient> ws_;

  // One I2S write's worth of samples. writePcm() takes int16_t*, and the
  // WebSocket payload is not guaranteed 2-byte aligned, so frames are copied
  // through here instead of being reinterpret_cast'd (ESP32-C3/S3 faults on
  // unaligned loads).
  int16_t pcm_[512] = {};
  uint8_t carry_ = 0;  // a trailing odd byte held for the next frame
  bool hasCarry_ = false;

  bool opened_ = false;
  bool gotMeta_ = false;
  bool gotAudio_ = false;
  bool ended_ = false;
  bool failed_ = false;
  char errorCode_[24] = {};

  uint32_t bytesPlayed_ = 0;
  uint32_t frames_ = 0;
  uint32_t startedAtMs_ = 0;
  uint32_t lastDataMs_ = 0;
  std::atomic<bool> stopRequested_{false};
};

}  // namespace VoiceRelay
