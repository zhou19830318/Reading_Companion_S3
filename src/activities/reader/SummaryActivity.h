#pragma once

#include <BookLibraryClient.h>
#include <BookSummaryClient.h>
#include <CloudConfig.h>
#include <I18n.h>
#include <esp_heap_caps.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "activities/Activity.h"

class SummaryActivity final : public Activity {
 public:
  SummaryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath, uint16_t spineIndex,
                  bool wholeBook)
      : Activity("Summary", renderer, mappedInput),
        bookPath_(std::move(bookPath)),
        spineIndex_(spineIndex),
        wholeBook_(wholeBook) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State : uint8_t { Loading, Ready, Failed };
  enum class Kind : uint8_t { Summary, Ingest };

  struct RequestData {
    char url[CloudConfig::RELAY_URL_SIZE] = {};
    char token[CloudConfig::TOKEN_SIZE] = {};
    char deviceId[CloudConfig::RELAY_DEVICE_ID_SIZE] = {};
    char bookId[24] = {};
  };

  struct RelayRequest {
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};
    std::atomic<bool> uploading{true};
    std::atomic<uint8_t> progress{0};
    RequestData* data = nullptr;
    BookSummary::Response* response = nullptr;
    BookLibrary::IngestResponse ingestResponse;
    Kind kind = Kind::Summary;
    bool upload = false;
    bool wholeBook = false;
    int spineIndex = 0;
    BookSummary::Client::Result result = BookSummary::Client::Result::BadConfig;
    BookSummary::Client::Stats stats;
    BookLibrary::Client::Result ingestResult = BookLibrary::Client::Result::BadConfig;
    BookLibrary::Client::Stats ingestStats;
    std::string bookPath;

    ~RelayRequest() {
      heap_caps_free(data);
      heap_caps_free(response);
    }
  };

  State state_ = State::Loading;
  bool started_ = false;
  std::string bookPath_;
  uint16_t spineIndex_ = 0;
  bool wholeBook_ = false;
  char bookId_[BookLibrary::BOOK_ID_CAP] = {};
  bool identifyingBook_ = false;
  uint8_t identifyPercent_ = 0;
  bool summaryRetriedAfterIngest_ = false;
  bool lastIngestUploading_ = true;
  uint8_t lastIngestPercent_ = 0;
  StrId failText_ = StrId::STR_SUMMARY_FAILED;
  std::shared_ptr<RelayRequest> request_;
  uint32_t deadlineMs_ = 0;
  size_t scrollLine_ = 0;
  size_t totalLines_ = 0;
  int visibleLines_ = 0;

  void startRequest();
  void startSummaryRequest();
  void startIngest(bool upload);
  static void relayTask(void* arg);
  static void hashProgress(void* ctx, uint8_t percent);
  static void ingestProgress(void* ctx, const BookLibrary::Client::Progress& progress);
  void handleCompletion();
  void fail(StrId text);
  void promptForWifi();
};
