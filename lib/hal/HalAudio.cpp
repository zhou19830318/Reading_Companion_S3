#include "HalAudio.h"

#include <Logging.h>

#if defined(ONEPAGE_S3)
#include <driver/i2s_common.h>
#include <driver/i2s_std.h>

#include "HalGPIO.h"
#endif

HalAudio HalAudio::instance;

#if defined(ONEPAGE_S3)

namespace {
// 256 samples = 16 ms at 16 kHz. Kept small so beep() does not need a large
// stack/heap buffer; the I2S DMA ring absorbs the chunk boundaries.
int16_t toneBuf[256];

constexpr uint32_t BEEP_DRAIN_MS = 30;
}  // namespace

bool HalAudio::begin() {
  if (ready) {
    return true;
  }

  pinMode(AUDIO_EN, OUTPUT);
  digitalWrite(AUDIO_EN, LOW);
  enabled = false;
  LOG_INF("AUD", "amp ground off, creating I2S TX channel");

  i2s_chan_handle_t tx = nullptr;
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  // 8 x 512 samples = 256 ms of slack at 16 kHz. TTS arrives in 256 ms
  // WebSocket frames and the Wi-Fi stack can pause for a beacon interval; a
  // 64 ms ring underran mid-sentence, a 256 ms ring covers one whole frame.
  chanCfg.dma_desc_num = 8;
  chanCfg.dma_frame_num = 512;
  chanCfg.auto_clear = true;

  esp_err_t err = i2s_new_channel(&chanCfg, &tx, nullptr);
  if (err != ESP_OK) {
    LOG_ERR("AUD", "i2s_new_channel failed: %d", static_cast<int>(err));
    return false;
  }

  LOG_INF("AUD", "I2S TX channel created, configuring std mode");
  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  stdCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.bclk = static_cast<gpio_num_t>(AUDIO_BCLK);
  stdCfg.gpio_cfg.ws = static_cast<gpio_num_t>(AUDIO_LRCLK);
  stdCfg.gpio_cfg.dout = static_cast<gpio_num_t>(AUDIO_DIN);
  stdCfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.invert_flags.mclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.bclk_inv = false;
  stdCfg.gpio_cfg.invert_flags.ws_inv = false;

  err = i2s_channel_init_std_mode(tx, &stdCfg);
  if (err != ESP_OK) {
    LOG_ERR("AUD", "init_std_tx failed: %d", static_cast<int>(err));
    i2s_del_channel(tx);
    return false;
  }

  LOG_INF("AUD", "std mode configured, enabling channel");
  err = i2s_channel_enable(tx);
  if (err != ESP_OK) {
    LOG_ERR("AUD", "i2s_channel_enable failed: %d", static_cast<int>(err));
    i2s_del_channel(tx);
    return false;
  }

  txHandle = tx;
  ready = true;
  LOG_INF("AUD", "MAX98357A I2S TX up at %lu Hz", static_cast<unsigned long>(SAMPLE_RATE));
  return true;
}

void HalAudio::end() {
  setEnabled(false);
  if (txHandle != nullptr) {
    auto* tx = static_cast<i2s_chan_handle_t>(txHandle);
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    txHandle = nullptr;
  }
  ready = false;
}

void HalAudio::setEnabled(bool on) {
  pinMode(AUDIO_EN, OUTPUT);
  digitalWrite(AUDIO_EN, on ? HIGH : LOW);
  enabled = on;
}

bool HalAudio::beep(uint16_t frequencyHz, uint16_t durationMs) {
  if (!begin() || frequencyHz == 0) {
    return false;
  }

  LOG_INF("AUD", "beep %u Hz %u ms, amp on", frequencyHz, durationMs);
  setEnabled(true);
  delay(2);  // MAX98357A turn-on / ground switch settle

  const bool ok = tone(frequencyHz, durationMs);
  if (ok) {
    delay(durationMs + BEEP_DRAIN_MS);
  }
  setEnabled(false);
  LOG_INF("AUD", "beep done");
  return ok;
}

bool HalAudio::tone(uint16_t frequencyHz, uint16_t durationMs, int16_t amplitude) {
  if (!begin() || frequencyHz == 0) {
    return false;
  }

  const uint32_t totalSamples = static_cast<uint32_t>(SAMPLE_RATE) * durationMs / 1000;
  if (totalSamples == 0) {
    return false;
  }

  const uint32_t halfPeriod = static_cast<uint32_t>(SAMPLE_RATE) / (2u * frequencyHz);
  const uint32_t period = halfPeriod < 1 ? 2 : halfPeriod * 2;
  uint32_t phase = 0;
  const int16_t AMPLITUDE = amplitude;

  constexpr uint32_t TONE_BUF_SAMPLES = sizeof(toneBuf) / sizeof(toneBuf[0]);
  uint32_t written = 0;
  while (written < totalSamples) {
    const uint32_t remaining = totalSamples - written;
    const uint32_t chunk = remaining < TONE_BUF_SAMPLES ? remaining : TONE_BUF_SAMPLES;
    for (uint32_t i = 0; i < chunk; i++) {
      toneBuf[i] = (phase < halfPeriod) ? AMPLITUDE : -AMPLITUDE;
      if (++phase >= period) {
        phase = 0;
      }
    }
    size_t bytesWritten = 0;
    const esp_err_t err = i2s_channel_write(static_cast<i2s_chan_handle_t>(txHandle), toneBuf, chunk * sizeof(int16_t),
                                            &bytesWritten, 1000);
    if (err != ESP_OK) {
      LOG_ERR("AUD", "i2s_channel_write failed: %d", static_cast<int>(err));
      return false;
    }
    written += chunk;
  }
  return true;
}

bool HalAudio::writePcm(const int16_t* samples, size_t count) {
  if (!begin() || samples == nullptr || count == 0) {
    return false;
  }

  lastPcmMs = millis();
  size_t bytesWritten = 0;
  const esp_err_t err = i2s_channel_write(static_cast<i2s_chan_handle_t>(txHandle), samples, count * sizeof(int16_t),
                                          &bytesWritten, 1000);
  if (err != ESP_OK) {
    LOG_ERR("AUD", "i2s_channel_write failed: %d", static_cast<int>(err));
    return false;
  }
  return true;
}

bool HalAudio::isStreamActive(unsigned long withinMs) const {
  return ready && lastPcmMs != 0 && millis() - lastPcmMs < withinMs;
}

#else  // !ONEPAGE_S3

bool HalAudio::begin() { return false; }
void HalAudio::end() {}
void HalAudio::setEnabled(bool) {}
bool HalAudio::isStreamActive(unsigned long) const { return false; }
bool HalAudio::beep(uint16_t, uint16_t) { return false; }
bool HalAudio::tone(uint16_t, uint16_t, int16_t) { return false; }
bool HalAudio::writePcm(const int16_t*, size_t) { return false; }

#endif  // ONEPAGE_S3
