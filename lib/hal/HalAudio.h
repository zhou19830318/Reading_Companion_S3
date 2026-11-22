#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

// MAX98357A I2S audio output HAL (OnePage V3.1).
//
// The amplifier's ground is switched by Q2 (AUDIO_EN/GPIO43, high = powered),
// so the amp draws no current while the reader is idle. I2S runs on I2S_NUM_1:
// the PDM microphone owns I2S_NUM_0.
//
// The HAL is intentionally small: enable/disable the amp and push 16 kHz mono
// PCM. beep() is a blocking convenience used for UI feedback; streamed playback
// (TTS, audiobooks) should call setEnabled(true), writePcm() in chunks, then
// setEnabled(false).
class HalAudio {
 public:
  static constexpr uint32_t SAMPLE_RATE = 16000;

  // Allocate/configure the I2S TX channel. Idempotent; safe if audio is absent.
  bool begin();

  // Release the I2S channel (the amp is switched off first).
  void end();

  bool isReady() const { return ready; }

  // Q2 amp-ground switch. High = amplifier powered.
  void setEnabled(bool enabled);
  bool isEnabled() const { return enabled; }

  // Blocking square-wave beep for self-tests. Returns false if the I2S
  // channel could not be initialised or the write failed.
  bool beep(uint16_t frequencyHz = 1200, uint16_t durationMs = 60);

  // Non-blocking tone: queues the samples into the I2S DMA ring and returns.
  // The caller owns the amplifier (setEnabled) — switching Q2 per tone clicks
  // audibly and wastes the settle delay, so UI feedback keeps the amp up for a
  // short tail and lets poll-style code switch it off. Returns false if the
  // channel is unavailable or the write failed.
  bool tone(uint16_t frequencyHz, uint16_t durationMs, int16_t amplitude = 6000);

  // Wait for queued audio to leave the DMA ring (busy-wait free: the I2S
  // peripheral plays in the background, this only delays).
  void drain(uint16_t ms) { delay(ms); }

  // True if streamed PCM was pushed within the last `withinMs`. UiSound uses
  // this to avoid cutting the amp under a stream it does not own: its own
  // tones expire after a fixed tail, but a TTS/voice stream keeps writing.
  bool isStreamActive(unsigned long withinMs = 300) const;

  // Push signed 16-bit mono PCM. The caller owns amp enable/disable. Blocks
  // until the samples are queued into the I2S DMA ring.
  bool writePcm(const int16_t* samples, size_t count);

  static HalAudio& getInstance() { return instance; }

 private:
  HalAudio() = default;
  HalAudio(const HalAudio&) = delete;
  HalAudio& operator=(const HalAudio&) = delete;

  static HalAudio instance;

  bool ready = false;
  bool enabled = false;
  unsigned long lastPcmMs = 0;  // last writePcm() timestamp
  void* txHandle = nullptr;     // i2s_chan_handle_t; opaque to keep this header IDF-free
};

#define AUDIO HalAudio::getInstance()
