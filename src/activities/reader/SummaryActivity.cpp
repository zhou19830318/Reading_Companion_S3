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
  job->result = BookSummary::Client::get(data->url, data->token, data->deviceId, data->bookId, job->wholeBook,
                                         job->spineIndex, *job->response, &job->stats, &job->cancel);
  job->done.store(true, std::memory_order_release);
  WIFI_IDLE.release();
  vTaskDelete(nullptr);
}

void SummaryActivity::startRequest() {
  CLOUD_CFG.load();
  if (SETTINGS.voiceRelayEnabled == 0 || CLOUD_CFG.relayUrl[0] == '\0' || CLOUD_CFG.relayToken[0] == '\0' ||
      CLOUD_CFG.relayDeviceId[0] == '\0') {
    fail(StrId::STR_VOICE_RELAY_NOT_CONFIGURED);
    return;
  }
  const std::string bookId = readCachedBookId(bookPath_);
  if (bookId.empty()) {
    fail(StrId::STR_AI_NOT_UPLOADED);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    promptForWifi();
    return;
  }

  auto job = std::make_shared<RelayRequest>();
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
  std::snprintf(job->data->bookId, sizeof(job->data->bookId), "%s", bookId.c_str());

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

void SummaryActivity::handleCompletion() {
  if (request_ == nullptr) return;
  if (!request_->done.load(std::memory_order_acquire)) {
    if (millis() >= deadlineMs_) {
      request_->cancel.store(true, std::memory_order_release);
      fail(StrId::STR_AI_REQUEST_TIMEOUT);
    }
    return;
  }
  const auto result = request_->result;
  const int status = request_->stats.httpStatus;
  if (result == BookSummary::Client::Result::Cancelled) return;
  if (result != BookSummary::Client::Result::Ok) {
    switch (result) {
      case BookSummary::Client::Result::NotFound:
        fail(StrId::STR_AI_NOT_UPLOADED);
        break;
      case BookSummary::Client::Result::NotReady:
        fail(StrId::STR_AI_INDEXING);
        break;
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
    const char* message = tr(STR_SUMMARY_LOADING);
    const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, message)) / 2;
    renderer.drawText(fontId, x, contentTop + contentHeight / 2 - lineHeight / 2, message);
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
