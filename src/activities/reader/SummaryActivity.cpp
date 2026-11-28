#include "SummaryActivity.h"

#include <CloudConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookIdentity.h"

void SummaryActivity::onEnter() {
  Activity::onEnter();
  WIFI_IDLE.arm();
  if (WiFi.status() != WL_CONNECTED) {
    promptForWifi();
    return;
  }
  requestUpdate();
}

void SummaryActivity::onExit() {
  if (request_) request_->cancel.store(true, std::memory_order_release);
  Activity::onExit();
}

bool SummaryActivity::preventAutoSleep() { return state_ == State::Loading; }

void SummaryActivity::promptForWifi() {
  started_ = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             fail(StrId::STR_STT_NO_NETWORK);
                           } else {
                             state_ = State::Loading;
                             started_ = false;
                           }
                           requestUpdate();
                         });
}

void SummaryActivity::fail(const StrId text) {
  failText_ = text;
  state_ = State::Failed;
  requestUpdate();
}

void SummaryActivity::relayTask(void* arg) {
  std::unique_ptr<std::shared_ptr<RelayRequest>> holder(static_cast<std::shared_ptr<RelayRequest>*>(arg));
  auto job = std::move(*holder);
  auto* data = static_cast<RequestData*>(job->data);
  if (job->kind == Kind::Ingest) {
    job->ingestResult = BookLibrary::Client::ensureReady(
        data->url, data->token, data->deviceId, data->bookId, job->bookPath.c_str(), job->upload,
        &SummaryActivity::ingestProgress, job.get(), job->ingestResponse, &job->ingestStats, &job->cancel);
  } else {
    job->result = BookSummary::Client::get(data->url, data->token, data->deviceId, data->bookId, job->wholeBook,
                                           job->spineIndex, *job->response, &job->stats, &job->cancel);
  }
  job->done.store(true, std::memory_order_release);
  WIFI_IDLE.release();
  vTaskDelete(nullptr);
}

void SummaryActivity::hashProgress(void* ctx, const uint8_t percent) {
  auto* self = static_cast<SummaryActivity*>(ctx);
  self->identifyPercent_ = percent;
  self->requestUpdate(true);
}

void SummaryActivity::ingestProgress(void* ctx, const BookLibrary::Client::Progress& progress) {
  auto* job = static_cast<RelayRequest*>(ctx);
  job->uploading.store(progress.uploading, std::memory_order_relaxed);
  job->progress.store(progress.percent, std::memory_order_relaxed);
}

void SummaryActivity::startRequest() {
  CLOUD_CFG.load();
  if (SETTINGS.voiceRelayEnabled == 0 || CLOUD_CFG.relayUrl[0] == '\0' || CLOUD_CFG.relayToken[0] == '\0' ||
      CLOUD_CFG.relayDeviceId[0] == '\0') {
    fail(StrId::STR_VOICE_RELAY_NOT_CONFIGURED);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    promptForWifi();
    return;
  }

  summaryRetriedAfterIngest_ = false;
  std::string bookId = readCachedBookId(bookPath_);
  if (bookId.empty()) {
    identifyingBook_ = true;
    identifyPercent_ = 0;
    requestUpdateAndWait();
    bookId = getBookId(bookPath_, &SummaryActivity::hashProgress, this);
    identifyingBook_ = false;
    requestUpdate(true);
  }
  if (bookId.empty()) {
    fail(StrId::STR_AI_IDENTIFY_FAILED);
    return;
  }
  std::snprintf(bookId_, sizeof(bookId_), "%s", bookId.c_str());
  startSummaryRequest();
}

void SummaryActivity::startSummaryRequest() {
  auto job = std::make_shared<RelayRequest>();
  job->kind = Kind::Summary;
  job->wholeBook = wholeBook_;
  job->spineIndex = spineIndex_;
  job->data = static_cast<RequestData*>(heap_caps_malloc(sizeof(RequestData), MALLOC_CAP_SPIRAM));
  if (job->data == nullptr) {
    job->data = static_cast<RequestData*>(heap_caps_malloc(sizeof(RequestData), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  job->response =
      static_cast<BookSummary::Response*>(heap_caps_malloc(sizeof(BookSummary::Response), MALLOC_CAP_SPIRAM));
  if (job->response == nullptr) {
    job->response = static_cast<BookSummary::Response*>(
        heap_caps_malloc(sizeof(BookSummary::Response), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (job->data == nullptr || job->response == nullptr) {
    fail(StrId::STR_MIC_OOM);
    return;
  }
  new (job->data) RequestData{};
  new (job->response) BookSummary::Response{};
  std::snprintf(job->data->url, sizeof(job->data->url), "%s", CLOUD_CFG.relayUrl);
  std::snprintf(job->data->token, sizeof(job->data->token), "%s", CLOUD_CFG.relayToken);
  std::snprintf(job->data->deviceId, sizeof(job->data->deviceId), "%s", CLOUD_CFG.relayDeviceId);
  std::snprintf(job->data->bookId, sizeof(job->data->bookId), "%s", bookId_);

  auto* arg = new (std::nothrow) std::shared_ptr<RelayRequest>(job);
  if (arg == nullptr) {
    fail(StrId::STR_MIC_OOM);
    return;
  }
  WIFI_IDLE.hold();
  if (xTaskCreate(&SummaryActivity::relayTask, "BookSummary", 12288, arg, 1, nullptr) != pdPASS) {
    WIFI_IDLE.release();
    delete arg;
    fail(StrId::STR_MIC_OOM);
    return;
  }
  request_ = std::move(job);
  deadlineMs_ = millis() + BookSummary::Client::TOTAL_TIMEOUT_MS;
}

void SummaryActivity::startIngest(const bool upload) {
  auto job = std::make_shared<RelayRequest>();
  job->kind = Kind::Ingest;
  job->upload = upload;
  job->bookPath = bookPath_;
  lastIngestUploading_ = true;
  lastIngestPercent_ = 0;
  job->data = static_cast<RequestData*>(heap_caps_malloc(sizeof(RequestData), MALLOC_CAP_SPIRAM));
  if (job->data == nullptr) {
    job->data = static_cast<RequestData*>(heap_caps_malloc(sizeof(RequestData), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (job->data == nullptr) {
    fail(StrId::STR_MIC_OOM);
    return;
  }
  new (job->data) RequestData{};
  std::snprintf(job->data->url, sizeof(job->data->url), "%s", CLOUD_CFG.relayUrl);
  std::snprintf(job->data->token, sizeof(job->data->token), "%s", CLOUD_CFG.relayToken);
  std::snprintf(job->data->deviceId, sizeof(job->data->deviceId), "%s", CLOUD_CFG.relayDeviceId);
  std::snprintf(job->data->bookId, sizeof(job->data->bookId), "%s", bookId_);

  auto* arg = new (std::nothrow) std::shared_ptr<RelayRequest>(job);
  if (arg == nullptr) {
    fail(StrId::STR_MIC_OOM);
    return;
  }
  WIFI_IDLE.hold();
  if (xTaskCreate(&SummaryActivity::relayTask, "BookIngest", 12288, arg, 1, nullptr) != pdPASS) {
    WIFI_IDLE.release();
    delete arg;
    fail(StrId::STR_MIC_OOM);
    return;
  }
  request_ = std::move(job);
  deadlineMs_ = millis() + BookLibrary::Client::TOTAL_TIMEOUT_MS;
  requestUpdate();
}

void SummaryActivity::handleCompletion() {
  if (request_ == nullptr) return;
  if (request_->kind == Kind::Ingest) {
    const bool uploading = request_->uploading.load(std::memory_order_relaxed);
    const uint8_t progress = request_->progress.load(std::memory_order_relaxed);
    if (uploading != lastIngestUploading_ || progress != lastIngestPercent_) {
      lastIngestUploading_ = uploading;
      lastIngestPercent_ = progress;
      requestUpdate();
    }
  }
  if (!request_->done.load(std::memory_order_acquire)) {
    if (millis() >= deadlineMs_) {
      request_->cancel.store(true, std::memory_order_release);
      fail(request_->kind == Kind::Ingest ? StrId::STR_BOOK_INGEST_FAILED : StrId::STR_AI_REQUEST_TIMEOUT);
    }
    return;
  }
  if (request_->kind == Kind::Ingest) {
    const auto result = request_->ingestResult;
    const int status = request_->ingestStats.httpStatus;
    if (result == BookLibrary::Client::Result::Cancelled) return;
    if (result == BookLibrary::Client::Result::Ok) {
      LOG_INF("SUMMARY", "book %s ready; retrying summary", bookId_);
      summaryRetriedAfterIngest_ = true;
      request_.reset();
      startSummaryRequest();
      return;
    }
    LOG_ERR("SUMMARY", "ingest failed: %s (http %d)", BookLibrary::Client::resultName(result), status);
    if (result == BookLibrary::Client::Result::Auth) {
      fail(StrId::STR_AUTH_FAILED);
    } else if (result == BookLibrary::Client::Result::NoWifi) {
      fail(StrId::STR_STT_NO_NETWORK);
    } else if (result == BookLibrary::Client::Result::TooLarge) {
      fail(StrId::STR_BOOK_TOO_LARGE);
    } else if (result == BookLibrary::Client::Result::Unsupported) {
      fail(StrId::STR_BOOK_UNSUPPORTED);
    } else if (result == BookLibrary::Client::Result::Timeout) {
      fail(StrId::STR_BOOK_INGEST_TIMEOUT);
    } else {
      fail(StrId::STR_BOOK_INGEST_FAILED);
    }
    return;
  }

  const auto result = request_->result;
  const int status = request_->stats.httpStatus;
  if (result == BookSummary::Client::Result::Cancelled) return;
  if (result != BookSummary::Client::Result::Ok) {
    switch (result) {
      case BookSummary::Client::Result::NotFound:
        if (!summaryRetriedAfterIngest_) {
          LOG_INF("SUMMARY", "book %s missing; uploading before retry", bookId_);
          startIngest(true);
        } else {
          fail(StrId::STR_BOOK_INGEST_FAILED);
        }
        return;
      case BookSummary::Client::Result::NotReady:
        if (!summaryRetriedAfterIngest_) {
          LOG_INF("SUMMARY", "book %s indexing; waiting before retry", bookId_);
          startIngest(false);
        } else {
          fail(StrId::STR_AI_INDEXING);
        }
        return;
      case BookSummary::Client::Result::Timeout:
        fail(StrId::STR_AI_REQUEST_TIMEOUT);
        break;
      case BookSummary::Client::Result::Auth:
        fail(StrId::STR_AUTH_FAILED);
        break;
      default:
        LOG_ERR("SUMMARY", "request failed: %s (http %d)", BookSummary::Client::resultName(result), status);
        fail(StrId::STR_SUMMARY_FAILED);
        break;
    }
    return;
  }
  scrollLine_ = 0;
  totalLines_ = 0;
  state_ = State::Ready;
  LOG_INF("SUMMARY", "ready: %u chars", static_cast<unsigned>(std::strlen(request_->response->text)));
  requestUpdate();
}

void SummaryActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (state_ == State::Loading) {
    if (!started_) {
      started_ = true;
      requestUpdateAndWait();
      startRequest();
      return;
    }
    handleCompletion();
    return;
  }
  if (state_ == State::Failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      request_.reset();
      scrollLine_ = 0;
      state_ = State::Loading;
      started_ = false;
      requestUpdate();
    }
    return;
  }

  const size_t pageStep = visibleLines_ > 0 ? static_cast<size_t>(visibleLines_) : 1;
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    scrollLine_ = scrollLine_ > 0 ? scrollLine_ - 1 : 0;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (scrollLine_ + 1 < totalLines_) ++scrollLine_;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const size_t nextPage = (scrollLine_ / pageStep + 1) * pageStep;
    scrollLine_ = std::min(totalLines_ > 0 ? totalLines_ - 1 : 0, nextPage);
    requestUpdate();
  }
}

void SummaryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const StrId heading = wholeBook_ ? StrId::STR_BOOK_INTRO : StrId::STR_CHAPTER_SUMMARY;
  const int headerY = screen.y + metrics.topPadding;
  GUI.drawHeader(renderer, Rect{screen.x, headerY, screen.width, metrics.headerHeight}, I18N.get(heading));

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = screen.y + screen.height - metrics.verticalSpacing - 28;
  const int contentHeight = std::max(0, contentBottom - contentTop);
  const int fontId = UI_10_FONT_ID;
  const int lineHeight = renderer.getLineHeight(fontId);
  visibleLines_ = lineHeight > 0 ? contentHeight / lineHeight : 0;

  if (state_ == State::Loading) {
    char status[96];
    if (identifyingBook_) {
      std::snprintf(status, sizeof(status), "%s %u%%", tr(STR_AI_IDENTIFYING), static_cast<unsigned>(identifyPercent_));
    } else if (request_ && request_->kind == Kind::Ingest) {
      const StrId label =
          request_->uploading.load(std::memory_order_relaxed) ? StrId::STR_BOOK_UPLOADING : StrId::STR_BOOK_INDEXING;
      std::snprintf(status, sizeof(status), "%s %u%%", I18N.get(label),
                    static_cast<unsigned>(request_->progress.load(std::memory_order_relaxed)));
    } else {
      std::snprintf(status, sizeof(status), "%s", tr(STR_SUMMARY_LOADING));
    }
    const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, status)) / 2;
    renderer.drawText(fontId, x, contentTop + contentHeight / 2 - lineHeight / 2, status);
  } else if (state_ == State::Failed) {
    const char* message = I18N.get(failText_);
    const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, message)) / 2;
    renderer.drawText(fontId, x, contentTop + contentHeight / 2 - lineHeight / 2, message);
  } else if (request_ && request_->response) {
    const auto& response = *request_->response;
    const std::vector<std::string> lines = renderer.wrappedText(fontId, response.text, screen.width, 512);
    totalLines_ = lines.size();
    const size_t maxFirst =
        totalLines_ > static_cast<size_t>(visibleLines_) ? totalLines_ - static_cast<size_t>(visibleLines_) : 0;
    scrollLine_ = std::min(scrollLine_, maxFirst);
    int y = contentTop;
    for (size_t i = scrollLine_; i < lines.size() && y + lineHeight <= contentBottom; ++i, y += lineHeight) {
      renderer.drawText(fontId, screen.x, y, lines[i].c_str());
    }
    const size_t page = visibleLines_ > 0 ? scrollLine_ / static_cast<size_t>(visibleLines_) + 1 : 1;
    const size_t pages =
        visibleLines_ > 0 ? (totalLines_ + static_cast<size_t>(visibleLines_) - 1) / static_cast<size_t>(visibleLines_)
                          : 1;
    char pageText[24];
    std::snprintf(pageText, sizeof(pageText), "%zu/%zu", page, pages);
    const int pageWidth = renderer.getTextWidth(fontId, pageText);
    renderer.drawText(fontId, screen.x + screen.width - pageWidth,
                      headerY + std::max(0, (metrics.headerHeight - lineHeight) / 2), pageText);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
