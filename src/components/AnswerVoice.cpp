#include "components/AnswerVoice.h"

#include <Logging.h>
#include <Memory.h>
#include <WifiIdleManager.h>

#include <cstring>

AnswerVoice::~AnswerVoice() {
  stop();
  if (task_ != nullptr) {
    vTaskDelete(task_);
    task_ = nullptr;
  }
}

const char* AnswerVoice::pickLang(const char* text) {
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != '\0'; ++p) {
    if (*p >= 0x80) return "zh";
  }
  return "en";
}

void AnswerVoice::start(const char* text, const char* lang, const char* thenText, const char* thenLang) {
  if (text == nullptr || text[0] == '\0') return;
  stop();
  auto copy = makeUniqueNoThrow<char[]>(strlen(text) + 1);
  if (!copy) {
    LOG_ERR("SPK", "OOM: text buffer");
    return;
  }
  strcpy(copy.get(), text);
  text_ = std::move(copy);
  snprintf(lang_, sizeof(lang_), "%s", lang != nullptr ? lang : pickLang(text));
  thenText_.reset();
  if (thenText != nullptr && thenText[0] != '\0') {
    auto then = makeUniqueNoThrow<char[]>(strlen(thenText) + 1);
    if (then) {
      strcpy(then.get(), thenText);
      thenText_ = std::move(then);
      snprintf(thenLang_, sizeof(thenLang_), "%s", thenLang != nullptr ? thenLang : pickLang(thenText));
    }
  }
  if (player_ == nullptr) {
    player_ = makeUniqueNoThrow<VoiceRelay::TtsPlayer>();
    if (!player_) {
      LOG_ERR("SPK", "OOM: TTS player");
      return;
    }
  }
  if (task_ == nullptr) {
    // 12 KB: the WebSockets TLS handshake runs on this task, and PSRAM is not
    // usable for task stacks (same budget as VoiceActivity's VoiceTts).
    if (xTaskCreate(&trampoline, "CardTts", 12288, this, 1, &task_) != pdPASS) {
      LOG_ERR("SPK", "TTS task create failed");
      task_ = nullptr;
      return;
    }
  }
  if (!held_) {
    WIFI_IDLE.hold();
    held_ = true;
  }
  cancelled_.store(false, std::memory_order_relaxed);
  done_.store(false, std::memory_order_relaxed);
  LOG_INF("SPK", "start (%u bytes, lang=%s)", static_cast<unsigned>(strlen(text)), lang_);
  xTaskNotifyGive(task_);
}

void AnswerVoice::loop() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (player_ != nullptr && text_ != nullptr) {
      const VoiceRelay::TtsPlayer::Result result = player_->speak(text_.get(), lang_);
      LOG_INF("SPK", "round: %s", VoiceRelay::TtsPlayer::resultName(result));
      if (result == VoiceRelay::TtsPlayer::Result::Ok && thenText_ != nullptr &&
          !cancelled_.load(std::memory_order_relaxed)) {
        const VoiceRelay::TtsPlayer::Result then = player_->speak(thenText_.get(), thenLang_);
        LOG_INF("SPK", "round2: %s", VoiceRelay::TtsPlayer::resultName(then));
      }
    }
    done_.store(true, std::memory_order_relaxed);
  }
}

void AnswerVoice::stop() {
  if (task_ == nullptr || done_.load(std::memory_order_relaxed)) {
    releaseHold();
    return;
  }
  cancelled_.store(true, std::memory_order_relaxed);
  if (player_ != nullptr) player_->stop();
  // speak() can sit in one I2S write (1 s timeout) or a TLS read; 3 s covers
  // those without freezing an exit for the whole stream cap.
  for (int i = 0; i < 300 && !done_.load(std::memory_order_relaxed); ++i) delay(10);
  if (!done_.load(std::memory_order_relaxed)) LOG_ERR("SPK", "stop timeout");
  releaseHold();
}

void AnswerVoice::tick() {
  if (held_ && done_.load(std::memory_order_relaxed)) releaseHold();
}

void AnswerVoice::releaseHold() {
  if (held_) {
    WIFI_IDLE.release();
    held_ = false;
  }
}