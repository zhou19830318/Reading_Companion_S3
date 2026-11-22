#pragma once

#include <TtsPlayer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>

// Spoken answers for the text-card activities (ask-AI, dictionary, word cards,
// review): one blocking TTS round on a dedicated task, the VoiceActivity
// pattern. The helper owns copies of the texts so the caller's reply buffer
// may be reused or freed as soon as start() returns.
//
// Threading: start()/stop()/tick() run on the main-loop task; only the private
// loop() runs on the TTS task. The radio hold is taken and released on the
// main task (tick() releases it once a round ends by itself).
class AnswerVoice {
 public:
  AnswerVoice() = default;
  ~AnswerVoice();
  AnswerVoice(const AnswerVoice&) = delete;
  AnswerVoice& operator=(const AnswerVoice&) = delete;

  // lang nullptr picks "zh" when the text has any non-ASCII byte, else "en".
  // thenText/thenLang append a second round (word pronunciation before the
  // card, say); it is skipped when the first round fails or is stopped.
  void start(const char* text, const char* lang = nullptr, const char* thenText = nullptr,
             const char* thenLang = nullptr);
  // Interrupts a running round and waits for the task to park (bounded).
  void stop();
  // Main-loop pump: drops the radio hold after a round that ended by itself.
  void tick();
  bool busy() const { return !done_.load(std::memory_order_relaxed); }

 private:
  static void trampoline(void* ctx) { static_cast<AnswerVoice*>(ctx)->loop(); }
  void loop();
  void releaseHold();
  static const char* pickLang(const char* text);

  std::unique_ptr<VoiceRelay::TtsPlayer> player_;
  TaskHandle_t task_ = nullptr;
  std::unique_ptr<char[]> text_;
  std::unique_ptr<char[]> thenText_;
  char lang_[4] = "zh";
  char thenLang_[4] = "zh";
  std::atomic<bool> done_{true};
  // speak() resets its own stop flag per call, so a stop that lands between
  // two rounds would otherwise let the second round dial anyway.
  std::atomic<bool> cancelled_{false};
  bool held_ = false;
};