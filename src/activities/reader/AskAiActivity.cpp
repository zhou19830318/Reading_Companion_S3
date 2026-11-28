#include "AskAiActivity.h"

// clang-format off
// HalStorage.h pulls SdFat macros that collide with lwip's ip4_addr.h unless
// seen first (same ordering constraint as VoiceActivity.cpp).
#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
// clang-format on
#include <ChatTime.h>
#include <CloudConfig.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SyncCodec.h>
#include <SyncOutbox.h>
#include <WiFi.h>
#include <WifiIdleManager.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/voice/VoiceActivity.h"
#include "components/CompanionMascot.h"
#include "components/CompanionUi.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookIdentity.h"

using Notes::NoteStore;

struct RelayRequest {
  enum class Kind : uint8_t { Query, Chat, Ingest };
  Kind kind = Kind::Query;
  StrId notice = StrId::STR_AI_UNCITED;
  std::atomic<bool> done{false};
  std::atomic<bool> cancel{false};
  std::atomic<bool> timedOut{false};
  std::atomic<bool> workerFinished{false};
  std::atomic<bool> uploading{true};
  std::atomic<uint8_t> ingestProgress{0};
  void* data = nullptr;
  ReaderQuery::Response* response = nullptr;
  char* reply = nullptr;
  ReaderQuery::Client::Result queryResult = ReaderQuery::Client::Result::BadConfig;
  VoiceRelay::ChatClient::Result chatResult = VoiceRelay::ChatClient::Result::BadConfig;
  BookLibrary::Client::Result ingestResult = BookLibrary::Client::Result::BadConfig;
  ReaderQuery::Client::Stats queryStats;
  VoiceRelay::ChatClient::Stats chatStats;
  BookLibrary::Client::Stats ingestStats;
  BookLibrary::IngestResponse ingestResponse;
  bool upload = false;
  bool truncated = false;
  std::string bookPath;

  ~RelayRequest() {
    heap_caps_free(data);
    heap_caps_free(response);
    heap_caps_free(reply);
  }
};

namespace {

uint32_t utf8FirstCodepoint(const char* data, const size_t size) {
  if (data == nullptr || size == 0) return 0;
  const auto* bytes = reinterpret_cast<const unsigned char*>(data);
  if (bytes[0] < 0x80) return bytes[0];
  if ((bytes[0] & 0xE0) == 0xC0 && size >= 2) return ((bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F);
  if ((bytes[0] & 0xF0) == 0xE0 && size >= 3) {
    return ((bytes[0] & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
  }
  if ((bytes[0] & 0xF8) == 0xF0 && size >= 4) {
    return ((bytes[0] & 0x07) << 18) | ((bytes[1] & 0x3F) << 12) | ((bytes[2] & 0x3F) << 6) | (bytes[3] & 0x3F);
  }
  return 0;
}

uint32_t utf8LastCodepoint(const char* data, const size_t size) {
  if (data == nullptr || size == 0) return 0;
  size_t start = size - 1;
  while (start > 0 && (static_cast<unsigned char>(data[start]) & 0xC0) == 0x80) --start;
  return utf8FirstCodepoint(data + start, size - start);
}

bool isCjkText(const uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

bool isCjkClosingPunct(const uint32_t cp) {
  switch (cp) {
    case 0xFF0C:  // ，
    case 0x3002:  // 。
    case 0xFF01:  // ！
    case 0xFF1F:  // ？
    case 0xFF1B:  // ；
    case 0xFF1A:  // ：
    case 0x3001:  // 、
    case 0xFF09:  // ）
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
      return true;
    default:
      return false;
  }
}

bool isCjkOpeningPunct(const uint32_t cp) {
  switch (cp) {
    case 0xFF08:  // （
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
      return true;
    default:
      return false;
  }
}

// Mirrors relay/book_ingest.py's gap removal and ParsedText's layout rule, so
// joining a line's word tokens does not insert spaces between Chinese glyphs.
bool cjkGapRemoved(const uint32_t previous, const uint32_t next) {
  return (isCjkText(previous) && isCjkText(next)) || (isCjkText(previous) && isCjkClosingPunct(next)) ||
         (isCjkOpeningPunct(previous) && isCjkText(next)) || (isCjkClosingPunct(previous) && isCjkText(next));
}

// Triangle-wave rule under a picked line: 3 px half-periods drawn as 1 px
// Bresenham diagonals, so a 440 px line costs ~147 drawLine calls instead of
// one per pixel. Amplitude 2 px reads as a wave without crowding the next
// line, and the full-line width comes from the page's own margins — no copy of
// the line's text is needed to know where it ends.
void drawSelectionRule(GfxRenderer& renderer, const int x, const int y, const int width) {
  constexpr int half = 3;  // period 6 px
  constexpr int amp = 2;
  if (width <= 0) return;
  for (int px = 0; px < width; px += half) {
    const int end = std::min(px + half, width);
    const bool down = ((px / half) % 2) == 0;
    renderer.drawLine(x + px, y + (down ? 0 : amp), x + end - 1, y + (down ? amp : 0), true);
  }
}

struct QueryRequestData {
  char url[CloudConfig::RELAY_URL_SIZE] = {};
  char token[CloudConfig::TOKEN_SIZE] = {};
  char deviceId[CloudConfig::RELAY_DEVICE_ID_SIZE] = {};
  char bookId[17] = {};
  char question[1024] = {};
  char selectedText[4001] = {};
  char intent[32] = {};
  char contextScope[32] = {};
  char language[8] = {};
  char passageIds[8][40] = {};
  const char* passagePtrs[8] = {};
  ReaderQuery::Request request = {};
};

struct ChatRequestData {
  char url[CloudConfig::RELAY_URL_SIZE] = {};
  char token[CloudConfig::TOKEN_SIZE] = {};
  char deviceId[CloudConfig::RELAY_DEVICE_ID_SIZE] = {};
  char prompt[VoiceRelay::CHAT_TEXT_CAP] = {};
};

struct IngestRequestData {
  char url[CloudConfig::RELAY_URL_SIZE] = {};
  char token[CloudConfig::TOKEN_SIZE] = {};
  char deviceId[CloudConfig::RELAY_DEVICE_ID_SIZE] = {};
  char bookId[BookLibrary::BOOK_ID_CAP] = {};
};

void ingestProgress(void* ctx, const BookLibrary::Client::Progress& progress) {
  auto* job = static_cast<RelayRequest*>(ctx);
  job->uploading.store(progress.uploading, std::memory_order_relaxed);
  job->ingestProgress.store(progress.percent, std::memory_order_relaxed);
}

void relayRequestTask(void* arg) {
  std::unique_ptr<std::shared_ptr<RelayRequest>> holder(static_cast<std::shared_ptr<RelayRequest>*>(arg));
  auto job = std::move(*holder);
  if (job->kind == RelayRequest::Kind::Query) {
    auto* data = static_cast<QueryRequestData*>(job->data);
    job->queryResult = ReaderQuery::Client::ask(data->url, data->token, data->deviceId, data->request, *job->response,
                                                &job->queryStats, &job->cancel);
  } else if (job->kind == RelayRequest::Kind::Ingest) {
    auto* data = static_cast<IngestRequestData*>(job->data);
    job->ingestResult = BookLibrary::Client::ensureReady(data->url, data->token, data->deviceId, data->bookId,
                                                         job->bookPath.c_str(), job->upload, &ingestProgress, job.get(),
                                                         job->ingestResponse, &job->ingestStats, &job->cancel);
  } else {
    auto* data = static_cast<ChatRequestData*>(job->data);
    job->chatResult = VoiceRelay::ChatClient::ask(data->url, data->token, data->deviceId, data->prompt, job->reply,
                                                  VoiceRelay::CHAT_TEXT_CAP + 1, job->truncated, nullptr, 0, nullptr,
                                                  &job->chatStats, &job->cancel);
  }
  job->workerFinished.store(true, std::memory_order_release);
  job->done.store(true, std::memory_order_release);
  WIFI_IDLE.release();
  job.reset();
  holder.reset();
  vTaskDelete(nullptr);
}

bool startRelayRequest(const std::shared_ptr<RelayRequest>& job) {
  auto* arg = new (std::nothrow) std::shared_ptr<RelayRequest>(job);
  if (arg == nullptr) return false;
  // The worker owns this hold until its bounded socket call unwinds, including
  // after the UI cancels and drops its own shared_ptr.
  WIFI_IDLE.hold();
  // HTTP/TLS uses more stack than the indexing workers; PSRAM cannot back task
  // stacks, so keep this in internal RAM like the other network tasks.
  if (xTaskCreate(&relayRequestTask, "AskRelay", 12288, arg, 1, nullptr) != pdPASS) {
    WIFI_IDLE.release();
    delete arg;
    return false;
  }
  return true;
}
}  // namespace

void AskAiActivity::setBookContext(const std::shared_ptr<Epub>& book, const char* bookBase, uint16_t spineIndex,
                                   uint16_t pageNumber, uint16_t pageCount) {
  book_ = book;
  bookBase_[0] = '\0';
  if (bookBase != nullptr) {
    snprintf(bookBase_, sizeof(bookBase_), "%s", bookBase);
  }
  spineIndex_ = spineIndex;
  pageNumber_ = pageNumber;
  pageCount_ = pageCount;
  bookId_[0] = '\0';
  queryPassageCount_ = 0;
}

void AskAiActivity::onEnter() {
  Activity::onEnter();
  // No radio and no big buffers on entry: the picker works offline, the link
  // is dialled only when a question is actually sent (ask-at-send, like the
  // note-mode fix in VoiceActivity), and prompt_/record_/lineBuf_ are allocated
  // lazily — always after the TLS dial has come back up. First paint comes
  // straight from the armed context. The idle manager gets armed so a finished
  // question powers the radio down once this screen goes quiet; ask-at-send
  // re-prompts for Wi-Fi after such a teardown, so nothing strands.
  WIFI_IDLE.arm();
  if (chapterDiscussionPending_) state_ = State::Asking;
  requestUpdate();
}

void AskAiActivity::startChapterDiscussion() {
  chapterDiscussionMode_ = true;
  chapterDiscussionPending_ = true;
}

void AskAiActivity::onExit() {
  cancelRelayRequest();
  voice_.stop();
  exitSelectionView();
  if (linePool_ != nullptr) {
    heap_caps_free(linePool_);
    linePool_ = nullptr;
  }
  if (queryResponse_ != nullptr) {
    heap_caps_free(queryResponse_);
    queryResponse_ = nullptr;
  }
  if (queryText_ != nullptr) {
    heap_caps_free(queryText_);
    queryText_ = nullptr;
  }
  if (querySelectedText_ != nullptr) {
    heap_caps_free(querySelectedText_);
    querySelectedText_ = nullptr;
  }
  linePoolCap_ = 0;
  lineCount_ = 0;
  Activity::onExit();
}

bool AskAiActivity::preventAutoSleep() { return state_ == State::Asking || voice_.busy(); }

void AskAiActivity::onPause() { voice_.stop(); }

void AskAiActivity::beginAsk(const char* question, Scope scope, const char* intent) {
  if (question == nullptr || question[0] == '\0') return;
  if (relayRequest_ != nullptr && !relayRequest_->workerFinished.load(std::memory_order_acquire)) {
    relayRequest_->cancel.store(true, std::memory_order_release);
    fail(tr(STR_AI_REQUEST_TIMEOUT));
    return;
  }
  cancelRelayRequest();
  relayDeadlineMs_ = 0;
  snprintf(question_, sizeof(question_), "%s", question);
  scope_ = scope;
  intent_ = (intent != nullptr && intent[0] != '\0') ? intent : "explain";
  saved_ = false;
  answerScroll_ = 0;
  failReason_ = nullptr;
  uncited_ = false;
  uncitedNotice_ = StrId::STR_AI_UNCITED;
  identifyingBook_ = false;
  queryRetriedAfterIngest_ = false;
  lastIngestUploading_ = true;
  lastIngestPercent_ = 0;
  if (queryText_ != nullptr) queryText_[0] = '\0';
  if (relayReply_ != nullptr) relayReply_[0] = '\0';
  if (queryResponse_ != nullptr) *queryResponse_ = ReaderQuery::Response{};
  // Drop everything the selection view held before any radio work: the page
  // and its glyph prewarm are the only internal-heap residents here, and the
  // class rule is that neither may meet the TLS handshake's dip.
  exitSelectionView();

  if (scope_ == Scope::Range && lineCount_ == 0) {
    // Retry path after a pick-time extraction failure: the normal flow
    // extracts before the selection screen, so reaching here with an empty
    // pool means the lines still have to be fetched (then picked again).
    if (!extractPageLines()) {
      fail(tr(STR_ASK_NO_TEXT));
      return;
    }
    enterSelecting();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    // Ask-at-send, not ask-at-entry: the preset list and the keyboard stay
    // usable offline, and cancelling the network list lands back on the picker
    // instead of finishing the activity — the shape of the note-mode fix in
    // VoiceActivity::startRecording.
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               // Chapter-end mode has no generic picker to fall
                               // back to: cancelling Wi-Fi returns to the page.
                               if (chapterDiscussionMode_) {
                                 finish();
                                 return;
                               }
                               state_ = State::Picking;
                               requestUpdate();
                               return;
                             }
                             beginRelayAsk();
                           });
    return;
  }
  beginRelayAsk();
}

bool AskAiActivity::extractPageLines() {
  if (!book_) {
    LOG_ERR("ASK", "no book context for selection");
    return false;
  }
  // PSRAM first: the pool lives from pick time through the TLS dial, so it
  // must not be internal DRAM (connect dips the internal heap to ~2.5 KB).
  if (linePool_ == nullptr) {
    linePool_ = static_cast<char*>(heap_caps_malloc(LINE_POOL_SIZE, MALLOC_CAP_SPIRAM));
    if (linePool_ == nullptr) {
      linePool_ = static_cast<char*>(heap_caps_malloc(LINE_POOL_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    linePoolCap_ = linePool_ != nullptr ? LINE_POOL_SIZE : 0;
    if (linePool_ == nullptr) {
      LOG_ERR("ASK", "OOM: line pool %u bytes", static_cast<unsigned>(LINE_POOL_SIZE));
      return false;
    }
  }

  const uint32_t heapBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t used = 0;
  int count = 0;
  bool full = false;
  {
    auto section = makeUniqueNoThrow<Section>(book_, spineIndex_, renderer);
    if (!section) {
      LOG_ERR("ASK", "OOM: Section");
      return false;
    }
    section->currentPage = static_cast<int>(pageNumber_);
    page_ = section->loadPageFromSectionFile();
    if (!page_) {
      LOG_ERR("ASK", "no cached page %u/%u", spineIndex_, pageNumber_);
      return false;
    }

    // Copy each PageLine's words into the pool as one line, word-separated.
    // A word that does not fit ends the line early (never a codepoint: words
    // are whole UTF-8 sequences), and a full pool or MAX_PAGE_LINES just stops
    // the walk — a truncated tail still yields a usable selection.
    for (const auto& el : page_->elements) {
      if (count >= MAX_PAGE_LINES || used >= linePoolCap_) {
        full = true;
        break;
      }
      if (el->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*el);
      const size_t start = used;
      if (line.getBlock() != nullptr) {
        uint32_t previousCp = 0;
        for (const auto& word : line.getBlock()->getWords()) {
          const uint32_t firstCp = utf8FirstCodepoint(word.data(), word.size());
          const bool sep = used > start && !cjkGapRemoved(previousCp, firstCp);
          if (used + (sep ? 1 : 0) + word.size() > linePoolCap_) {
            full = true;
            break;
          }
          if (sep) linePool_[used++] = ' ';
          memcpy(linePool_ + used, word.data(), word.size());
          used += word.size();
          previousCp = utf8LastCodepoint(word.data(), word.size());
        }
      }
      const size_t len = used - start;
      if (len == 0) {
        // A blank layout line has no entry to select — but a line whose FIRST
        // word did not fit the pool is not blank, it is the truncation point.
        // Stopping there keeps lineRefs_ a gapless prefix of the page's
        // non-blank lines, which is the invariant the selection view's element
        // walk relies on to index it 1:1.
        if (full) break;
        continue;
      }
      lineRefs_[count].off = static_cast<uint16_t>(start);
      lineRefs_[count].len = static_cast<uint16_t>(len);
      if (line.getBlock() != nullptr) {
        lineRefs_[count].passage = line.getBlock()->getPassage();
        lineRefs_[count].charStart = line.getBlock()->getPassageCharStart();
        lineRefs_[count].charEnd = line.getBlock()->getPassageCharEnd();
      } else {
        lineRefs_[count].passage = {};
        lineRefs_[count].charStart = 0;
        lineRefs_[count].charEnd = 0;
      }
      count++;
      if (full) break;
    }
  }  // Section released here: the retained delta below is the page alone
  lineCount_ = count;
  LOG_INF("ASK", "page lines: %d (%u bytes%s), selection page costs %d bytes internal", count,
          static_cast<unsigned>(used), full ? ", pool full" : "",
          static_cast<int>(heapBefore - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  return count > 0;
}

void AskAiActivity::enterSelecting() {
  selectCursor_ = 0;
  selectAnchor_ = -1;
  selectionQuoteSaved_ = false;
  scrollY_ = 0;
  state_ = State::Selecting;
  requestUpdate();
}

void AskAiActivity::exitSelectionView() {
  // Order matters only for the glyph cache: the scope's destructor clears it,
  // which is the reader's own convention at the end of a page render.
  prewarmScope_.reset();
  prewarmed_ = false;
  page_.reset();
  scrollY_ = 0;
}

bool AskAiActivity::relayConfigured() {
  if (SETTINGS.voiceRelayEnabled == 0) return false;
  CLOUD_CFG.load();  // NVS, idempotent; the same source the voice screen reads
  return CLOUD_CFG.relayUrl[0] != '\0' && CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
}

void AskAiActivity::ensureBookId() {
  if (bookId_[0] != '\0' || !book_) return;
  const std::string& path = book_->getPath();

  // Sidecar first: a book uploaded/downloaded through the relay already has
  // its id cached, regardless of size.
  const std::string cached = readCachedBookId(path);
  if (!cached.empty()) {
    std::snprintf(bookId_, sizeof(bookId_), "%s", cached.c_str());
    return;
  }

  // First fingerprint of an SD-copied book. The waiting frame is already up,
  // so hash behind it and repaint progress instead of falling back silently:
  // the read loop yields and feeds the watchdog per 16 KB chunk, and the
  // sidecar makes this a one-time cost. The old 4 MB cap meant a large book
  // that *was* indexed on the relay stayed "not indexed" on the device
  // forever, because the id was never computed.
  identifyingBook_ = true;
  identifyPercent_ = 0;
  requestUpdate(true);
  const std::string id = getBookId(path, &AskAiActivity::hashProgress, this);
  identifyingBook_ = false;
  requestUpdate(true);
  if (id.empty()) {
    uncitedNotice_ = StrId::STR_AI_IDENTIFY_FAILED;
    return;
  }
  std::snprintf(bookId_, sizeof(bookId_), "%s", id.c_str());
}

void AskAiActivity::hashProgress(void* ctx, const uint8_t percent) {
  auto* self = static_cast<AskAiActivity*>(ctx);
  self->identifyPercent_ = percent;
  LOG_DBG("ASK", "identifying book: %u%%", static_cast<unsigned>(percent));
  // loop() is the task doing the hashing, so its deferred requestUpdate()
  // would not run until the hash finished. Notify the render task directly;
  // the hash loop yields after every chunk, so the frame can paint.
  self->requestUpdate(true);
}

void AskAiActivity::addPassageSpan(const EpubPassage::Ref ref, const uint32_t charStart, const uint32_t charEnd) {
  if (!ref.valid()) return;
  for (int i = 0; i < queryPassageCount_; ++i) {
    if (queryPassages_[i].ref == ref) {
      if (charEnd > charStart) {
        queryPassages_[i].charStart = queryPassages_[i].charEnd > queryPassages_[i].charStart
                                          ? std::min(queryPassages_[i].charStart, charStart)
                                          : charStart;
        queryPassages_[i].charEnd = std::max(queryPassages_[i].charEnd, charEnd);
      }
      return;
    }
  }
  if (queryPassageCount_ >= MAX_QUERY_PASSAGES) return;
  queryPassages_[queryPassageCount_++] = {ref, charStart, charEnd};
}

void AskAiActivity::collectQueryPassages() {
  queryPassageCount_ = 0;
  if (scope_ == Scope::Range) {
    // The selection view already copied each line's source passage, so a
    // range costs no extra SD read.
    if (lineCount_ == 0 || selStart_ < 0 || selEnd_ < selStart_) return;
    for (int i = selStart_; i <= selEnd_ && i < lineCount_; ++i) {
      addPassageSpan(lineRefs_[i].passage, lineRefs_[i].charStart, lineRefs_[i].charEnd);
    }
    return;
  }

  if (!book_) return;
  std::unique_ptr<Page> loaded;
  const Page* page = page_.get();
  if (page == nullptr) {
    auto section = makeUniqueNoThrow<Section>(book_, spineIndex_, renderer);
    if (!section) {
      LOG_ERR("ASK", "OOM: Section for passage mapping");
      return;
    }
    section->currentPage = static_cast<int>(pageNumber_);
    loaded = section->loadPageFromSectionFile();
    page = loaded.get();
  }
  if (page == nullptr) return;

  for (const auto& el : page->elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine&>(*el).getBlock();
    if (block == nullptr) continue;
    addPassageSpan(block->getPassage(), block->getPassageCharStart(), block->getPassageCharEnd());
  }
}

bool AskAiActivity::ensureSelectedText() {
  if (linePool_ == nullptr || lineCount_ == 0 || selStart_ < 0 || selEnd_ < selStart_ || selEnd_ >= lineCount_) {
    return false;
  }
  constexpr size_t cap = SELECTED_TEXT_CAP;
  if (querySelectedText_ == nullptr) {
    querySelectedText_ = static_cast<char*>(heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM));
    if (querySelectedText_ == nullptr) {
      querySelectedText_ = static_cast<char*>(heap_caps_malloc(cap + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
  }
  if (querySelectedText_ == nullptr) {
    LOG_ERR("ASK", "OOM: selected text %u bytes", static_cast<unsigned>(cap + 1));
    return false;
  }

  size_t used = 0;
  for (int i = selStart_; i <= selEnd_ && used < cap; ++i) {
    const auto& line = lineRefs_[i];
    if (used > 0) {
      // A new source paragraph starts a new quote line; a layout wrap inside
      // the same paragraph is joined back, with a space only where the script
      // needs one (CJK punctuation/glyphs do not).
      if (i > selStart_ && line.passage != lineRefs_[i - 1].passage) {
        querySelectedText_[used++] = '\n';
      } else {
        const uint32_t previous = utf8LastCodepoint(querySelectedText_, used);
        const uint32_t next = utf8FirstCodepoint(linePool_ + line.off, line.len);
        if (!cjkGapRemoved(previous, next)) querySelectedText_[used++] = ' ';
      }
      if (used >= cap) break;
    }
    size_t take = std::min<size_t>(line.len, cap - used);
    while (take > 0 && (static_cast<unsigned char>(linePool_[line.off + take]) & 0xC0) == 0x80) --take;
    if (take == 0) break;
    memcpy(querySelectedText_ + used, linePool_ + line.off, take);
    used += take;
  }
  querySelectedText_[used] = '\0';
  return used > 0;
}

void AskAiActivity::saveSelectionQuote() {
  if (selectionQuoteSaved_ || scope_ != Scope::Range || bookId_[0] == '\0' || queryPassageCount_ == 0) return;
  if (!ensureSelectedText()) return;

  char passageId[40];
  if (EpubPassage::formatId(passageId, sizeof(passageId), bookId_, spineIndex_, queryPassages_[0].ref) == 0) {
    LOG_ERR("ASK", "selection quote: cannot format passage id");
    return;
  }

  auto op = makeUniqueNoThrow<char[]>(Sync::MAX_OP_BYTES);
  if (!op) {
    LOG_ERR("ASK", "selection quote skipped: OOM (%u bytes)", static_cast<unsigned>(Sync::MAX_OP_BYTES));
    return;
  }
  char opId[VoiceRelay::REQUEST_ID_CAP];
  VoiceRelay::formatRequestId(opId, esp_random(), esp_random(), esp_random(), esp_random());
  char entityId[24];
  snprintf(entityId, sizeof(entityId), "an_%.16s", opId);

  const size_t len =
      Sync::buildNoteCreateOp(op.get(), Sync::MAX_OP_BYTES, opId, entityId, bookId_, passageId, querySelectedText_, "",
                              "highlight", spineIndex_, pageNumber_, pageCount_, ChatLog::nowEpochMs());
  if (len == 0) {
    LOG_ERR("ASK", "selection quote op build failed");
    return;
  }
  if (!Sync::append(op.get(), len)) {
    LOG_ERR("ASK", "selection quote append failed (%u bytes)", static_cast<unsigned>(len));
    return;
  }
  selectionQuoteSaved_ = true;
  LOG_INF("ASK", "selection quote queued: %s (%u bytes, %d passages)", entityId, static_cast<unsigned>(len),
          queryPassageCount_);
}

void AskAiActivity::beginRelayAsk() {
  if (!relayConfigured()) {
    fail(tr(STR_VOICE_RELAY_NOT_CONFIGURED));
    return;
  }
  // Paint the waiting screen and wait for the frame: SD fingerprinting and
  // passage extraction report progress before the network worker starts.
  state_ = State::Asking;
  requestUpdateAndWait();

  // Resolve the citation context behind the waiting frame (hashing a large
  // book can take a while on SD, so it reports progress). An empty id means
  // this file could not be identified; the relay's own reply then decides
  // whether the book is missing or merely still indexing.
  ensureBookId();
  collectQueryPassages();
  if (bookId_[0] == '\0') {
    LOG_INF("ASK", "book id unavailable: uncited Q&A");
    beginChat(uncitedNotice_);
    return;
  }

  LOG_INF("ASK", "book %s: %d passage(s) on this question", bookId_, queryPassageCount_);
  saveSelectionQuote();
  beginReaderQuery();
}

void AskAiActivity::cancelRelayRequest() {
  if (relayRequest_ == nullptr) return;
  relayRequest_->cancel.store(true, std::memory_order_relaxed);
  relayRequest_.reset();
  relayDeadlineMs_ = 0;
  LOG_INF("ASK", "relay request cancelled; late output will be discarded");
}

void AskAiActivity::beginChat(const StrId notice) {
  if (!buildPrompt()) {
    fail(tr(STR_ASK_NO_TEXT));
    return;
  }

  auto job = std::shared_ptr<RelayRequest>(new (std::nothrow) RelayRequest());
  if (!job) {
    fail(tr(STR_MIC_OOM));
    return;
  }
  job->kind = RelayRequest::Kind::Chat;
  job->notice = notice;
  auto* data = static_cast<ChatRequestData*>(heap_caps_malloc(sizeof(ChatRequestData), MALLOC_CAP_SPIRAM));
  if (data == nullptr) {
    data =
        static_cast<ChatRequestData*>(heap_caps_malloc(sizeof(ChatRequestData), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (data == nullptr) {
    LOG_ERR("ASK", "OOM: chat request %u bytes", static_cast<unsigned>(sizeof(ChatRequestData)));
    fail(tr(STR_MIC_OOM));
    return;
  }
  new (data) ChatRequestData{};
  job->data = data;
  snprintf(data->url, sizeof(data->url), "%s", CLOUD_CFG.relayUrl);
  snprintf(data->token, sizeof(data->token), "%s", CLOUD_CFG.relayToken);
  snprintf(data->deviceId, sizeof(data->deviceId), "%s", CLOUD_CFG.relayDeviceId);
  snprintf(data->prompt, sizeof(data->prompt), "%s", prompt_.get());

  job->reply = static_cast<char*>(heap_caps_malloc(VoiceRelay::CHAT_TEXT_CAP + 1, MALLOC_CAP_SPIRAM));
  if (job->reply == nullptr) {
    job->reply =
        static_cast<char*>(heap_caps_malloc(VoiceRelay::CHAT_TEXT_CAP + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (job->reply == nullptr) {
    LOG_ERR("ASK", "OOM: relay reply buffer %u bytes", static_cast<unsigned>(VoiceRelay::CHAT_TEXT_CAP + 1));
    fail(tr(STR_MIC_OOM));
    return;
  }
  job->reply[0] = '\0';
  if (!startRelayRequest(job)) {
    LOG_ERR("ASK", "relay chat task create failed");
    fail(tr(STR_MIC_OOM));
    return;
  }
  relayRequest_ = std::move(job);
  relayDeadlineMs_ = millis() + VoiceRelay::ChatClient::TOTAL_TIMEOUT_MS;
}

void AskAiActivity::beginReaderQuery() {
  int idCount = 0;
  for (auto& id : queryPassageIds_) id[0] = '\0';
  for (int i = 0; i < queryPassageCount_; ++i) {
    if (EpubPassage::formatId(queryPassageIds_[idCount], sizeof(queryPassageIds_[0]), bookId_, spineIndex_,
                              queryPassages_[i].ref) == 0) {
      continue;
    }
    queryPassageIdPtrs_[idCount] = queryPassageIds_[idCount];
    LOG_DBG("ASK", "passage %s [%u,%u)", queryPassageIds_[idCount], static_cast<unsigned>(queryPassages_[i].charStart),
            static_cast<unsigned>(queryPassages_[i].charEnd));
    idCount++;
  }

  const char* selectedText = "";
  if (scope_ == Scope::Range && ensureSelectedText()) selectedText = querySelectedText_;

  auto job = std::shared_ptr<RelayRequest>(new (std::nothrow) RelayRequest());
  if (!job) {
    fail(tr(STR_MIC_OOM));
    return;
  }
  job->kind = RelayRequest::Kind::Query;
  auto* data = static_cast<QueryRequestData*>(heap_caps_malloc(sizeof(QueryRequestData), MALLOC_CAP_SPIRAM));
  if (data == nullptr) {
    data = static_cast<QueryRequestData*>(
        heap_caps_malloc(sizeof(QueryRequestData), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (data == nullptr) {
    LOG_ERR("ASK", "OOM: reader query request %u bytes", static_cast<unsigned>(sizeof(QueryRequestData)));
    fail(tr(STR_MIC_OOM));
    return;
  }
  new (data) QueryRequestData{};
  job->data = data;
  snprintf(data->url, sizeof(data->url), "%s", CLOUD_CFG.relayUrl);
  snprintf(data->token, sizeof(data->token), "%s", CLOUD_CFG.relayToken);
  snprintf(data->deviceId, sizeof(data->deviceId), "%s", CLOUD_CFG.relayDeviceId);
  snprintf(data->bookId, sizeof(data->bookId), "%s", bookId_);
  snprintf(data->question, sizeof(data->question), "%s", question_);
  snprintf(data->selectedText, sizeof(data->selectedText), "%s", selectedText);
  snprintf(data->intent, sizeof(data->intent), "%s", intent_);
  snprintf(data->contextScope, sizeof(data->contextScope), "%s", contextScopeName());
  snprintf(data->language, sizeof(data->language), "%s", queryLanguage());
  for (int i = 0; i < idCount; ++i) {
    snprintf(data->passageIds[i], sizeof(data->passageIds[i]), "%s", queryPassageIds_[i]);
    data->passagePtrs[i] = data->passageIds[i];
  }
  data->request.bookId = data->bookId;
  data->request.spineIndex = spineIndex_;
  data->request.passageIds = data->passagePtrs;
  data->request.passageIdCount = static_cast<size_t>(idCount);
  data->request.selectedText = data->selectedText;
  data->request.question = data->question;
  data->request.intent = data->intent;
  data->request.contextScope = data->contextScope;
  data->request.language = data->language;

  job->response =
      static_cast<ReaderQuery::Response*>(heap_caps_malloc(sizeof(ReaderQuery::Response), MALLOC_CAP_SPIRAM));
  if (job->response == nullptr) {
    job->response = static_cast<ReaderQuery::Response*>(
        heap_caps_malloc(sizeof(ReaderQuery::Response), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (job->response == nullptr) {
    LOG_ERR("ASK", "OOM: ReaderQuery::Response %u bytes", static_cast<unsigned>(sizeof(ReaderQuery::Response)));
    fail(tr(STR_MIC_OOM));
    return;
  }
  new (job->response) ReaderQuery::Response{};
  if (!startRelayRequest(job)) {
    LOG_ERR("ASK", "reader query task create failed");
    fail(tr(STR_MIC_OOM));
    return;
  }
  relayRequest_ = std::move(job);
  relayDeadlineMs_ = millis() + ReaderQuery::Client::TOTAL_TIMEOUT_MS;
}

void AskAiActivity::beginIngest(const bool upload) {
  if (!book_) {
    fail(tr(STR_BOOK_INGEST_FAILED));
    return;
  }
  auto job = std::shared_ptr<RelayRequest>(new (std::nothrow) RelayRequest());
  if (!job) {
    fail(tr(STR_MIC_OOM));
    return;
  }
  job->kind = RelayRequest::Kind::Ingest;
  job->upload = upload;
  job->bookPath = book_->getPath();
  auto* data = static_cast<IngestRequestData*>(heap_caps_malloc(sizeof(IngestRequestData), MALLOC_CAP_SPIRAM));
  if (data == nullptr) {
    data = static_cast<IngestRequestData*>(
        heap_caps_malloc(sizeof(IngestRequestData), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  if (data == nullptr) {
    fail(tr(STR_MIC_OOM));
    return;
  }
  new (data) IngestRequestData{};
  job->data = data;
  snprintf(data->url, sizeof(data->url), "%s", CLOUD_CFG.relayUrl);
  snprintf(data->token, sizeof(data->token), "%s", CLOUD_CFG.relayToken);
  snprintf(data->deviceId, sizeof(data->deviceId), "%s", CLOUD_CFG.relayDeviceId);
  snprintf(data->bookId, sizeof(data->bookId), "%s", bookId_);
  if (!startRelayRequest(job)) {
    fail(tr(STR_MIC_OOM));
    return;
  }
  relayRequest_ = std::move(job);
  relayDeadlineMs_ = millis() + BookLibrary::Client::TOTAL_TIMEOUT_MS;
  requestUpdate();
}

void AskAiActivity::handleRelayCompletion() {
  if (state_ != State::Asking || relayRequest_ == nullptr) return;
  if (relayRequest_->timedOut.load(std::memory_order_acquire)) {
    relayRequest_->cancel.store(true, std::memory_order_release);
    const char* kind = relayRequest_->kind == RelayRequest::Kind::Query    ? "reader query"
                       : relayRequest_->kind == RelayRequest::Kind::Ingest ? "book ingest"
                                                                           : "relay chat";
    LOG_ERR("ASK", "%s request timed out; late output will be discarded", kind);
    if (relayRequest_->workerFinished.load(std::memory_order_acquire)) relayRequest_.reset();
    fail(tr(STR_AI_REQUEST_TIMEOUT));
    return;
  }
  if (relayRequest_->kind == RelayRequest::Kind::Ingest) {
    const bool uploading = relayRequest_->uploading.load(std::memory_order_relaxed);
    const uint8_t progress = relayRequest_->ingestProgress.load(std::memory_order_relaxed);
    if (uploading != lastIngestUploading_ || progress != lastIngestPercent_) {
      lastIngestUploading_ = uploading;
      lastIngestPercent_ = progress;
      requestUpdate();
    }
  }
  if (!relayRequest_->done.load(std::memory_order_acquire)) return;
  auto job = std::move(relayRequest_);
  relayDeadlineMs_ = 0;

  if (job->kind == RelayRequest::Kind::Ingest) {
    const auto result = job->ingestResult;
    const auto stats = job->ingestStats;
    if (result == BookLibrary::Client::Result::Cancelled) return;
    if (result == BookLibrary::Client::Result::Ok) {
      LOG_INF("ASK", "book %s ready; retrying cited query", bookId_);
      queryRetriedAfterIngest_ = true;
      beginReaderQuery();
      return;
    }
    LOG_ERR("ASK", "book ingest failed: %s (http %d)", BookLibrary::Client::resultName(result), stats.httpStatus);
    if (result == BookLibrary::Client::Result::Auth) {
      fail(tr(STR_AUTH_FAILED));
    } else if (result == BookLibrary::Client::Result::NoWifi) {
      fail(tr(STR_STT_NO_NETWORK));
    } else if (result == BookLibrary::Client::Result::TooLarge) {
      fail(tr(STR_BOOK_TOO_LARGE));
    } else if (result == BookLibrary::Client::Result::Unsupported) {
      fail(tr(STR_BOOK_UNSUPPORTED));
    } else if (result == BookLibrary::Client::Result::Timeout) {
      fail(tr(STR_BOOK_INGEST_TIMEOUT));
    } else {
      fail(tr(STR_BOOK_INGEST_FAILED));
    }
    return;
  }

  if (job->kind == RelayRequest::Kind::Chat) {
    const auto result = job->chatResult;
    const auto stats = job->chatStats;
    if (result == VoiceRelay::ChatClient::Result::Cancelled) return;
    if (result != VoiceRelay::ChatClient::Result::Ok) {
      LOG_ERR("ASK", "relay chat failed: %s (http %d)", VoiceRelay::ChatClient::resultName(result), stats.httpStatus);
      fail(result == VoiceRelay::ChatClient::Result::Timeout ? tr(STR_AI_REQUEST_TIMEOUT) : tr(STR_VOICE_RELAY_FAILED));
      return;
    }
    if (relayReply_ == nullptr) {
      relayReply_ = makeUniqueNoThrow<char[]>(VoiceRelay::CHAT_TEXT_CAP + 1);
      if (relayReply_ == nullptr) {
        LOG_ERR("ASK", "OOM: relay reply buffer %u bytes", static_cast<unsigned>(VoiceRelay::CHAT_TEXT_CAP + 1));
        fail(tr(STR_MIC_OOM));
        return;
      }
    }
    memcpy(relayReply_.get(), job->reply, strlen(job->reply) + 1);
    if (job->truncated) {
      LOG_ERR("ASK", "relay reply truncated at %u bytes", static_cast<unsigned>(VoiceRelay::CHAT_TEXT_CAP));
    }
    LOG_INF("ASK", "relay reply %u bytes (http %d)", static_cast<unsigned>(stats.responseBytes), stats.httpStatus);
    uncited_ = true;
    uncitedNotice_ = job->notice;
    answerScroll_ = 0;
    saveRecord();
    state_ = State::Answer;
    if (!chapterDiscussionMode_) voice_.start(relayReply_.get());
    requestUpdate();
    return;
  }

  const auto result = job->queryResult;
  const auto stats = job->queryStats;
  if (result == ReaderQuery::Client::Result::Cancelled) return;
  if (queryResponse_ == nullptr) {
    queryResponse_ =
        static_cast<ReaderQuery::Response*>(heap_caps_malloc(sizeof(ReaderQuery::Response), MALLOC_CAP_SPIRAM));
    if (queryResponse_ == nullptr) {
      queryResponse_ = static_cast<ReaderQuery::Response*>(
          heap_caps_malloc(sizeof(ReaderQuery::Response), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (queryResponse_ == nullptr) {
      LOG_ERR("ASK", "OOM: ReaderQuery::Response %u bytes", static_cast<unsigned>(sizeof(ReaderQuery::Response)));
      fail(tr(STR_MIC_OOM));
      return;
    }
  }
  *queryResponse_ = *job->response;
  if (result != ReaderQuery::Client::Result::Ok) {
    switch (result) {
      case ReaderQuery::Client::Result::NotUploaded:
        if (!queryRetriedAfterIngest_) {
          LOG_INF("ASK", "book %s not uploaded: uploading before retry", bookId_);
          beginIngest(true);
        } else {
          LOG_INF("ASK", "book %s still missing after ingest: falling back to uncited chat", bookId_);
          beginChat(StrId::STR_AI_NOT_UPLOADED);
        }
        return;
      case ReaderQuery::Client::Result::Indexing:
        if (!queryRetriedAfterIngest_) {
          LOG_INF("ASK", "book %s indexing: waiting before retry", bookId_);
          beginIngest(false);
        } else {
          LOG_INF("ASK", "book %s still indexing after wait: falling back to uncited chat", bookId_);
          beginChat(StrId::STR_AI_INDEXING);
        }
        return;
      case ReaderQuery::Client::Result::PassageMissing:
        LOG_INF("ASK", "book %s passage ids missing: falling back to uncited chat", bookId_);
        beginChat(StrId::STR_AI_PASSAGE_MISMATCH);
        return;
      case ReaderQuery::Client::Result::LibraryUnavailable:
        LOG_INF("ASK", "relay library route unavailable: falling back to uncited chat");
        beginChat(StrId::STR_AI_LIBRARY_UNAVAILABLE);
        return;
      default:
        break;
    }
    LOG_ERR("ASK", "reader query failed: %s (http %d, %u bytes)", ReaderQuery::Client::resultName(result),
            stats.httpStatus, static_cast<unsigned>(stats.responseBytes));
    fail(result == ReaderQuery::Client::Result::Timeout ? tr(STR_AI_REQUEST_TIMEOUT) : tr(STR_VOICE_RELAY_FAILED));
    return;
  }

  composeQueryAnswer();
  enqueueAiResult();
  answerScroll_ = 0;
  saveRecord();
  state_ = State::Answer;
  if (!chapterDiscussionMode_) voice_.start(answerText());
  requestUpdate();
}

const char* AskAiActivity::contextScopeName() const {
  switch (scope_) {
    case Scope::Range:
      return "selection";
    case Scope::Chapter:
      return "current_chapter";
    case Scope::Page:
    default:
      return "current_passage";
  }
}

const char* AskAiActivity::queryLanguage() const {
  // The relay accepts zh/en; the UI language only picks which one it is.
  const Language lang = I18N.getLanguage();
  return lang == Language::ZH_HANS || lang == Language::ZH_HANT ? "zh" : "en";
}

void AskAiActivity::composeQueryAnswer() {
  constexpr size_t DISPLAY_CAP = 16384;
  if (queryText_ == nullptr) {
    queryText_ = static_cast<char*>(heap_caps_malloc(DISPLAY_CAP, MALLOC_CAP_SPIRAM));
    if (queryText_ == nullptr) {
      queryText_ = static_cast<char*>(heap_caps_malloc(DISPLAY_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
  }
  if (queryText_ == nullptr || queryResponse_ == nullptr) {
    LOG_ERR("ASK", "OOM: composed answer");
    return;
  }
  char* out = queryText_;
  size_t used = 0;
  auto append = [&](const char* text) {
    if (used >= DISPLAY_CAP - 1) return;
    const size_t len = std::strlen(text);
    size_t take = std::min(len, DISPLAY_CAP - 1 - used);
    if (take < len) {
      while (take > 0 && (static_cast<unsigned char>(text[take]) & 0xC0) == 0x80) --take;
    }
    memcpy(out + used, text, take);
    used += take;
    out[used] = '\0';
  };
  auto appendLabel = [&](const StrId id) {
    append("[");
    append(I18n::getInstance().get(id));
    append("]\n");
  };

  if ((queryResponse_->warnings & ReaderQuery::WARN_MODEL_OUTPUT_NOT_JSON) != 0) {
    // The relay's parser could not recover any segments and wrapped the raw
    // model text (often a malformed JSON object) as inference. Showing that
    // verbatim is worse than asking for one more try.
    append(tr(STR_AI_MODEL_OUTPUT_UNSTRUCTURED));
    LOG_INF("ASK", "model output was not structured; showing retry notice");
    return;
  }

  if ((queryResponse_->warnings & ReaderQuery::WARN_CITATION_DROPPED) != 0) {
    appendLabel(StrId::STR_AI_CITATION_DROPPED);
    append("\n");
  }

  int citationNumber = 0;
  for (uint8_t i = 0; i < queryResponse_->segmentCount && used < DISPLAY_CAP - 1; ++i) {
    const auto& segment = queryResponse_->segments[i];
    switch (segment.source) {
      case ReaderQuery::Source::Book:
        appendLabel(StrId::STR_AI_SOURCE_BOOK);
        break;
      case ReaderQuery::Source::External:
        appendLabel(StrId::STR_AI_SOURCE_EXTERNAL);
        break;
      case ReaderQuery::Source::Inference:
      default:
        appendLabel(StrId::STR_AI_SOURCE_INFERENCE);
        break;
    }
    if (segment.textOffset < queryResponse_->textLen) {
      size_t len = segment.textLen;
      if (segment.textOffset + len > queryResponse_->textLen) len = queryResponse_->textLen - segment.textOffset;
      if (len > DISPLAY_CAP - 2 - used) {
        len = DISPLAY_CAP - 2 - used;
        // Never end the composed text on half a codepoint.
        while (len > 0 && (static_cast<unsigned char>(queryResponse_->text[segment.textOffset + len]) & 0xC0) == 0x80) {
          --len;
        }
      }
      memcpy(out + used, queryResponse_->text + segment.textOffset, len);
      used += len;
      out[used] = '\0';
    }
    for (uint8_t c = 0; c < segment.citationCount; ++c) {
      const uint8_t index = static_cast<uint8_t>(segment.firstCitation + c);
      if (index >= queryResponse_->citationCount) break;
      char marker[12];
      snprintf(marker, sizeof(marker), " [%d]", ++citationNumber);
      append(marker);
    }
    append("\n\n");
  }

  if (citationNumber > 0) {
    appendLabel(StrId::STR_AI_CITATIONS);
    int number = 0;
    for (uint8_t i = 0; i < queryResponse_->citationCount && used < DISPLAY_CAP - 1; ++i) {
      char line[8];
      snprintf(line, sizeof(line), "%d. ", ++number);
      append(line);
      append(queryResponse_->citations[i].quote);
      append("\n");
    }
  }
}

const char* AskAiActivity::answerText() const {
  if (!uncited_ && queryText_ != nullptr) return queryText_;
  return relayReply_ != nullptr ? relayReply_.get() : "";
}

void AskAiActivity::enqueueAiResult() {
  if (queryResponse_ == nullptr || bookId_[0] == '\0' || queryPassageCount_ == 0) return;

  // One op object is bounded by the outbox line cap. Allocated after the TLS
  // round, so the internal-heap peak never meets the handshake.
  auto op = makeUniqueNoThrow<char[]>(Sync::MAX_OP_BYTES);
  if (!op) {
    LOG_ERR("ASK", "sync enqueue skipped: OOM (%u bytes)", static_cast<unsigned>(Sync::MAX_OP_BYTES));
    return;
  }

  char opId[VoiceRelay::REQUEST_ID_CAP];
  VoiceRelay::formatRequestId(opId, esp_random(), esp_random(), esp_random(), esp_random());
  char entityId[24];
  snprintf(entityId, sizeof(entityId), "ai_%.16s", opId);

  int idCount = 0;
  for (int i = 0; i < queryPassageCount_; ++i) {
    if (queryPassageIds_[i][0] == '\0') continue;
    queryPassageIdPtrs_[idCount++] = queryPassageIds_[i];
  }

  const size_t len =
      ReaderQuery::buildAiResultOp(op.get(), Sync::MAX_OP_BYTES, opId, entityId, bookId_, queryPassageIdPtrs_,
                                   static_cast<size_t>(idCount), question_, ChatLog::nowEpochMs(), *queryResponse_);
  if (len == 0) {
    LOG_ERR("ASK", "sync op build failed (%d passages)", idCount);
    return;
  }
  if (!Sync::append(op.get(), len)) {
    LOG_ERR("ASK", "sync outbox append failed (%u bytes)", static_cast<unsigned>(len));
    return;
  }
  LOG_INF("ASK", "ai_result queued for sync: %s (%u bytes)", entityId, static_cast<unsigned>(len));
}

void AskAiActivity::startDiscussionVoice() {
  auto voice = std::make_unique<VoiceActivity>(renderer, mappedInput);
  voice->setQuestionInput(StrId::STR_AI_DISCUSS_VOICE_TITLE, StrId::STR_AI_DISCUSS_VIEWPOINT);
  // The viewpoint is the round's input, not a keyboard draft: hand it back as
  // soon as STT lands so the interaction stays spoken, like Pigeon chat.
  voice->setAutoSendTranscript(true);
  // The question was just chosen with Left/Right; go straight to listening
  // rather than asking for a second Confirm on the voice screen.
  voice->setAutoStartCapture(true);
  startActivityForResult(std::move(voice), [this](const ActivityResult& result) {
    const auto* typed = std::get_if<KeyboardResult>(&result.data);
    if (typed != nullptr && !typed->text.empty()) {
      submitDiscussionViewpoint(typed->text.c_str());
    } else {
      requestUpdate();  // cancelled: keep the chapter answer and choices up
    }
  });
}

void AskAiActivity::submitDiscussionViewpoint(const char* viewpoint) {
  if (!chapterDiscussionMode_ || viewpoint == nullptr || viewpoint[0] == '\0') return;
  const char* question = I18N.get(kDiscussionQuestions[discussionIndex_]);
  snprintf(discussionInput_, sizeof(discussionInput_), "%s\n\n%s：%s\n\n%s", question, tr(STR_AI_DISCUSS_MY_VIEW),
           viewpoint, tr(STR_AI_DISCUSS_RESPOND));
  LOG_INF("ASK", "chapter discussion: viewpoint for question %d (%u bytes)", discussionIndex_,
          static_cast<unsigned>(strlen(viewpoint)));
  // "apply" keeps the server's current_chapter retrieval while the wording
  // asks it to engage with the reader's own view instead of summarizing.
  beginAsk(discussionInput_, Scope::Chapter, "apply");
}

void AskAiActivity::openCitations() {
  citationCursor_ = 0;
  citationError_ = nullptr;
  state_ = State::Citations;
  requestUpdate();
}

bool AskAiActivity::findCitationPage(const ReaderQuery::Citation& citation, uint16_t& page) const {
  if (!book_) return false;
  const char* suffix = std::strrchr(citation.passageId, ':');
  if (suffix == nullptr || suffix[1] == '\0') return false;

  EpubPassage::Ref ref;
  const char* digits = nullptr;
  if (suffix[1] == 'p') {
    ref.kind = EpubPassage::Kind::Paragraph;
    digits = suffix + 2;
  } else if (suffix[1] == 'l' && suffix[2] == 'i') {
    ref.kind = EpubPassage::Kind::ListItem;
    digits = suffix + 3;
  } else {
    return false;
  }
  if (digits == nullptr || *digits < '0' || *digits > '9') return false;
  uint32_t index = 0;
  for (const char* p = digits; *p >= '0' && *p <= '9'; ++p) {
    index = index * 10 + static_cast<uint32_t>(*p - '0');
    if (index > 0xFFFF) return false;
  }
  ref.index = static_cast<uint16_t>(index);

  // The page cache has no passage index, so an explicit jump walks the
  // chapter's pages until a line carrying this anchor shows up. It runs only
  // on an explicit Confirm, with the citation list already painted.
  auto section = makeUniqueNoThrow<Section>(book_, citation.spineIndex, renderer);
  if (!section) {
    LOG_ERR("ASK", "OOM: Section for citation jump");
    return false;
  }
  const auto pageCount = section->getCachedPageCount();
  if (!pageCount) return false;
  for (int p = 0; p < static_cast<int>(*pageCount); ++p) {
    section->currentPage = p;
    auto loaded = section->loadPageFromSectionFile();
    if (!loaded) continue;
    for (const auto& element : loaded->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& block = static_cast<const PageLine&>(*element).getBlock();
      if (block != nullptr && block->getPassage() == ref) {
        page = static_cast<uint16_t>(p);
        LOG_INF("ASK", "citation %s -> spine %u page %u", citation.passageId,
                static_cast<unsigned>(citation.spineIndex), static_cast<unsigned>(p));
        return true;
      }
    }
  }
  return false;
}

void AskAiActivity::jumpToCitation(const int index) {
  if (queryResponse_ == nullptr || index < 0 || index >= queryResponse_->citationCount) return;
  uint16_t page = 0;
  if (!findCitationPage(queryResponse_->citations[index], page)) {
    LOG_ERR("ASK", "citation not locatable: %s", queryResponse_->citations[index].passageId);
    citationError_ = tr(STR_AI_CITATION_LOST);
    requestUpdate();
    return;
  }
  ActivityResult result;
  result.data =
      ProgressChangeResult{static_cast<int>(queryResponse_->citations[index].spineIndex), static_cast<int>(page)};
  setResult(std::move(result));
  finish();
}

bool AskAiActivity::buildPrompt() {
  if (!book_) {
    LOG_ERR("ASK", "no book context");
    return false;
  }
  // Allocated here — with the link already up — so the 1.5 KB request buffer
  // never sits on the internal heap across the TLS dial (plan §3.5).
  if (!prompt_) {
    prompt_ = makeUniqueNoThrow<char[]>(VoiceRelay::CHAT_TEXT_CAP);
    if (!prompt_) {
      LOG_ERR("ASK", "OOM: prompt buffer");
      return false;
    }
  }
  char* const promptBuf = prompt_.get();
  // Model-facing instruction, not UI text: the translated preset question is
  // what carries the answer's language, so this stays out of the i18n tables.
  static constexpr char INSTRUCTION[] =
      "You are a reading assistant on an e-reader. Answer in the language of the question. "
      "Plain text only, no markdown, at most 120 words.\n";
  const size_t cap = VoiceRelay::CHAT_TEXT_CAP;
  // The title goes in whole. A fixed-size copy of it (the old char title[65])
  // cut CJK metadata titles mid-codepoint, and an ill-formed byte in the
  // message makes the gateway close the socket before it can answer. A title
  // too long to fit is caught by the head >= cap test below instead of being
  // silently chopped.
  const int head =
      snprintf(promptBuf, cap, "%sBook: %s\nQuestion: %s\nText:\n", INSTRUCTION, book_->getTitle().c_str(), question_);
  if (head <= 0 || static_cast<size_t>(head) >= cap) {
    LOG_ERR("ASK", "prompt header overflow (%d)", head);
    return false;
  }
  size_t used = static_cast<size_t>(head);
  size_t budget = cap - used - 1;
  bool any = false;

  if (scope_ == Scope::Range) {
    // Join the picked lines straight from the selection pool: no SD, no
    // decode — extractPageLines() already paid that cost before the dial.
    if (linePool_ == nullptr || lineCount_ == 0 || selStart_ < 0 || selEnd_ < selStart_ || selEnd_ >= lineCount_) {
      LOG_ERR("ASK", "range selection invalid (%d..%d of %d)", selStart_, selEnd_, lineCount_);
      return false;
    }
    for (int i = selStart_; i <= selEnd_ && budget > 0; i++) {
      const LineRef ref = lineRefs_[i];
      if (ref.len == 0) continue;
      if (any) {
        // The separator spends from the same budget the line bytes do — size
        // take only after it, or a line that exactly fills the budget wraps
        // the unsigned budget and the next memcpy runs past the buffer (same
        // crash as the page loop below, 2026-10-17).
        if (budget <= 1) break;
        promptBuf[used++] = '\n';
        budget--;
        promptBuf[used] = '\0';
      }
      size_t take = std::min(budget, static_cast<size_t>(ref.len));
      if (take < ref.len) {
        // Never cut a UTF-8 codepoint in half: back off a continuation byte.
        while (take > 0 && (static_cast<unsigned char>(linePool_[ref.off + take]) & 0xC0) == 0x80) take--;
      }
      if (take == 0) break;
      memcpy(promptBuf + used, linePool_ + ref.off, take);
      used += take;
      budget -= take;
      promptBuf[used] = '\0';
      any = true;
    }
    LOG_DBG("ASK", "range context lines %d..%d: %u bytes left", selStart_, selEnd_, static_cast<unsigned>(budget));
    return any;
  }

  // Section reads from the book's cache file at the armed position; it is
  // built here (after the link is up) and released on return, so its page
  // decode peak is transient and never meets the handshake's.
  auto section = makeUniqueNoThrow<Section>(book_, spineIndex_, renderer);
  if (!section) {
    LOG_ERR("ASK", "OOM: Section");
    return false;
  }
  const int firstPage = static_cast<int>(pageNumber_);
  int lastPage = firstPage;
  if (scope_ == Scope::Chapter) {
    if (const auto cachedCount = section->getCachedPageCount()) {
      lastPage = std::min(firstPage + MAX_CONTEXT_PAGES - 1, static_cast<int>(*cachedCount) - 1);
    }
  }

  for (int page = firstPage; page <= lastPage && budget > 0; page++) {
    section->currentPage = page;
    const std::string text = section->getTextFromSectionFile();
    if (text.empty()) continue;  // image-only page
    if (any) {
      // The separator spends from the same budget the page bytes do. Sizing
      // take against the pre-separator budget let a page that exactly filled
      // the budget overflow by one byte and wrap the unsigned budget to
      // SIZE_MAX; the loop then memcpy'd the next page (961 bytes) far past
      // the request buffer, smashing the heap (Guru Meditation, 2026-10-17).
      if (budget <= 1) break;
      promptBuf[used++] = '\n';
      budget--;
      promptBuf[used] = '\0';
    }
    size_t take = std::min(budget, text.size());
    // Never cut a UTF-8 codepoint in half: back off a continuation byte.
    while (take > 0 && (static_cast<unsigned char>(text[take]) & 0xC0) == 0x80) take--;
    if (take == 0) break;
    memcpy(promptBuf + used, text.c_str(), take);
    used += take;
    budget -= take;
    promptBuf[used] = '\0';
    any = true;
    LOG_DBG("ASK", "context page %d: %u bytes (%u left)", page, static_cast<unsigned>(take),
            static_cast<unsigned>(budget));
  }
  return any;
}

void AskAiActivity::saveRecord() {
  // Both scratch buffers are first touched here — after the answer settled,
  // long after the TLS dial — so none of this sits on the internal heap
  // across the handshake. Freed with the activity (unique_ptr members).
  if (!record_) record_ = makeUniqueNoThrow<char[]>(RECORD_CAP);
  if (!lineBuf_) lineBuf_ = makeUniqueNoThrow<char[]>(NoteStore::LINE_CAP);
  if (!record_ || !lineBuf_) {
    LOG_ERR("ASK", "record skipped: OOM (%u + %u bytes)", static_cast<unsigned>(RECORD_CAP),
            static_cast<unsigned>(NoteStore::LINE_CAP));
    saved_ = false;
    return;
  }
  const char* reply = answerText();
  snprintf(record_.get(), RECORD_CAP, "%s%s%s\n%s%s%s", tr(STR_ASK_Q), tr(STR_NOTE_SEP), question_, tr(STR_ASK_A),
           tr(STR_NOTE_SEP), reply);
  const int64_t now = ChatLog::nowEpochMs();
  // One append per settled round trip — the same SD write discipline as a
  // dictated voice note. Failure only loses the record, never the answer on
  // screen, so it is logged instead of failing the state machine.
  saved_ = NoteStore::append(bookBase_, static_cast<uint64_t>(now), spineIndex_, pageNumber_, pageCount_, record_.get(),
                             lineBuf_.get(), NoteStore::LINE_CAP);
  if (saved_) {
    LOG_INF("ASK", "record saved to %s (spine %u page %u/%u)", bookBase_, spineIndex_, pageNumber_, pageCount_);
  } else {
    LOG_ERR("ASK", "note append failed (book=%s)", bookBase_);
  }
}

void AskAiActivity::fail(const char* reason) {
  LOG_ERR("ASK", "failed: %s", reason != nullptr ? reason : "?");
  failReason_ = reason != nullptr ? reason : tr(STR_VOICE_RELAY_FAILED);
  state_ = State::Failed;
  requestUpdate();
}

void AskAiActivity::loop() {
  voice_.tick();
  // esp_http_client_read() can stay inside one transport read while a peer
  // trickles bytes, so the worker's between-read checks alone are not a hard
  // deadline. Expire the visible round here and let the worker unwind later.
  if (state_ == State::Asking && relayRequest_ != nullptr && relayDeadlineMs_ != 0 &&
      !relayRequest_->done.load(std::memory_order_acquire) && static_cast<int32_t>(millis() - relayDeadlineMs_) >= 0) {
    relayRequest_->timedOut.store(true, std::memory_order_release);
    relayRequest_->cancel.store(true, std::memory_order_release);
    relayRequest_->done.store(true, std::memory_order_release);
  }
  handleRelayCompletion();

  // Chapter-end entry: the first frame is already the waiting screen, so the
  // network detour (or the blocking round, when Wi-Fi is up) starts on this
  // tick instead of showing the generic AI-book question list first.
  if (chapterDiscussionPending_) {
    chapterDiscussionPending_ = false;
    beginAsk(tr(STR_Q_CHAPTER_DISCUSSION), Scope::Chapter, "chapter_summary");
    return;
  }
  // Front Left/Right mirror the side Up/Down on every list/scroll state, so
  // the bottom row is never a dead end. The pair flips with the nav axis
  // (INVERTED / LANDSCAPE_CCW) to stay on the keys mapLabels() paints 上/下 over.
  const bool navSwapped = mappedInput.isNavDirectionSwapped();
  const auto prevKey = navSwapped ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextKey = navSwapped ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;

  // Back leaves from any state. During Asking it cancels the worker; its bounded
  // socket call may finish later, but the activity no longer owns that result.
  // The range picker is the exception: it steps back to the question list.
  //
  // The finish itself runs on the release edge (EpubReaderMenu's rule): the
  // reader also treats a Back release as "go home", so finishing on the press
  // edge would hand it the orphaned release and kick the user out of the book.
  // The press only arms the finish — a child that cancels on its own press
  // edge (VoiceActivity) leaves such a release behind, and with backArmed_
  // still false it is ignored.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    voice_.stop();
    if (state_ == State::Selecting) {
      exitSelectionView();  // the page and its glyph prewarm are per-selection
      state_ = State::Picking;
      requestUpdate();
      return;
    }
    if (state_ == State::Citations) {
      state_ = State::Answer;  // the answer is still on screen behind the picker
      requestUpdate();
      return;
    }
    backArmed_ = true;
    return;
  }
  if (backArmed_ && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    backArmed_ = false;
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (state_ == State::Answer) {
    if (chapterDiscussionMode_) {
      // The reader's request: Left/Right pick one of the system's open
      // questions, Confirm opens voice capture. Up/Down still scroll the
      // chapter answer, so the existing window stays readable.
      if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        discussionIndex_ = (discussionIndex_ + DISCUSSION_QUESTION_COUNT - 1) % DISCUSSION_QUESTION_COUNT;
        requestUpdate();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        discussionIndex_ = (discussionIndex_ + 1) % DISCUSSION_QUESTION_COUNT;
        requestUpdate();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
          mappedInput.wasPressed(MappedInputManager::Button::Power)) {
        startDiscussionVoice();
        return;
      }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) || mappedInput.wasPressed(prevKey)) {
      if (answerScroll_ > 0) answerScroll_--;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasPressed(nextKey)) {
      answerScroll_++;  // clamped by drawWindow() at render time
      requestUpdate();
    } else if (!chapterDiscussionMode_ && (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
                                           mappedInput.wasPressed(MappedInputManager::Button::Power))) {
      // The record is already written; open the citation picker when there is
      // one, otherwise back to the picker for another question.
      if (!uncited_ && queryResponse_ != nullptr && queryResponse_->citationCount > 0) {
        openCitations();
      } else {
        voice_.stop();
        state_ = State::Picking;
        selectedIndex_ = 0;
        requestUpdate();
      }
    }
    return;
  }

  if (state_ == State::Citations) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) || mappedInput.wasPressed(prevKey)) {
      if (citationCursor_ > 0) {
        citationCursor_--;
        citationError_ = nullptr;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasPressed(nextKey)) {
      if (!uncited_ && queryResponse_ != nullptr && citationCursor_ + 1 < queryResponse_->citationCount) {
        citationCursor_++;
        citationError_ = nullptr;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Power)) {
      jumpToCitation(citationCursor_);
      return;
    }
    return;
  }

  if (state_ == State::Failed) {
    // Retry keeps the same question, scope and intent — the network detour, if
    // any, runs again inside beginAsk().
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Power)) {
      beginAsk(question_, scope_, intent_);
    }
    return;
  }

  // ── Selecting ───────────────────────────────────────────────────
  if (state_ == State::Selecting) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) || mappedInput.wasPressed(prevKey)) {
      if (selectCursor_ > 0) selectCursor_--;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasPressed(nextKey)) {
      if (selectCursor_ + 1 < lineCount_) selectCursor_++;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Power)) {
      if (selectAnchor_ < 0) {
        // First Confirm marks the range start; the band grows to the cursor.
        selectAnchor_ = selectCursor_;
        requestUpdate();
      } else {
        selStart_ = std::min(selectAnchor_, selectCursor_);
        selEnd_ = std::max(selectAnchor_, selectCursor_);
        beginAsk(question_, Scope::Range, intent_);
      }
    }
    return;
  }

  // ── Picking ─────────────────────────────────────────────────────
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) || mappedInput.wasPressed(prevKey)) {
    if (selectedIndex_ > 0) {
      selectedIndex_--;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) || mappedInput.wasPressed(nextKey)) {
    if (selectedIndex_ + 1 < ROW_COUNT) {
      selectedIndex_++;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
      mappedInput.wasPressed(MappedInputManager::Button::Power)) {
    const Scope scope = kRowScope[selectedIndex_];
    if (selectedIndex_ == ROW_CUSTOM) {
      // Custom question through VoiceActivity's question-input mode: speak it
      // (one STT call) or press the left key and type it. The preset rows
      // above cost no STT call, which is the point of the presets.
      auto voice = std::make_unique<VoiceActivity>(renderer, mappedInput);
      voice->setQuestionInput();
      startActivityForResult(std::move(voice), [this](const ActivityResult& result) {
        const auto* typed = std::get_if<KeyboardResult>(&result.data);
        if (typed != nullptr && !typed->text.empty()) {
          beginAsk(typed->text.c_str(), kRowScope[ROW_CUSTOM], "custom");
        } else {
          requestUpdate();  // cancelled: repaint the picker
        }
      });
      return;
    }
    if (scope == Scope::Range) {
      // Range presets extract this page's lines first (PSRAM pool, still no
      // radio), then open the selection screen; the dial starts only after
      // the user confirms the range.
      snprintf(question_, sizeof(question_), "%s", I18N.get(kRowLabels[selectedIndex_]));
      scope_ = Scope::Range;
      intent_ = kRowIntent[selectedIndex_];
      if (!extractPageLines()) {
        fail(tr(STR_ASK_NO_TEXT));
        return;
      }
      enterSelecting();
      return;
    }
    beginAsk(I18N.get(kRowLabels[selectedIndex_]), scope, kRowIntent[selectedIndex_]);
  }
}

size_t AskAiActivity::drawWindow(const int fontId, const char* text, const int x, const int y, const int width,
                                 const int capacity, size_t& firstLine) {
  // Re-wrapped per repaint, exactly like VoiceActivity::drawTextWindow: the
  // answer lives in the session's shared buffer, and orientation decides the
  // width, so caching lines would need an orientation-keyed invalidation for
  // no measurable gain at e-ink frame rates.
  const std::vector<std::string> lines = renderer.wrappedText(fontId, text, width, WRAP_MAX_LINES);
  const size_t total = lines.size();
  if (total == 0 || capacity <= 0) {
    firstLine = 0;
    return total;
  }
  const size_t maxFirst = total > static_cast<size_t>(capacity) ? total - static_cast<size_t>(capacity) : 0;
  if (firstLine > maxFirst) firstLine = maxFirst;
  const size_t end = std::min(total, firstLine + static_cast<size_t>(capacity));
  int lineY = y;
  for (size_t i = firstLine; i < end; i++, lineY += renderer.getLineHeight(fontId)) {
    renderer.drawText(fontId, x, lineY, lines[i].c_str());
  }
  return total;
}

// Draws the page into the selection window at [viewTop, viewTop+viewHeight).
// Called twice for the first paint (scan, then real) and once per repaint
// afterwards — see the prewarm block in render().
void AskAiActivity::renderSelectionView(const int viewTop, const int viewHeight) {
  const int fontId = SETTINGS.getReaderFontId();
  const int lineH = renderer.getLineHeight(fontId);
  // Same horizontal margins the reader laid this page out with: the cached
  // elements are viewport-relative, so reproducing them is exactly what puts
  // every line back where it was on the reading page.
  int bezelTop = 0, bezelRight = 0, bezelBottom = 0, bezelLeft = 0;
  renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
  const int marginL = bezelLeft + SETTINGS.screenMargin;
  const int marginR = bezelRight + SETTINGS.screenMargin;
  const int rightX = renderer.getScreenWidth() - marginR;
  const int caretX = std::max(0, marginL - 7);
  const int viewBottom = viewTop + viewHeight;

  // Keep the cursor's line whole inside the window. The window slides over the
  // page's viewport-relative Y coordinates; scrollY_ is never negative and
  // never leaves the cursor less than a full line from either edge.
  int cursorY = -1;
  {
    int idx = -1;
    for (const auto& el : page_->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const auto& probe = static_cast<const PageLine&>(*el);
      if (probe.getBlock() == nullptr || probe.getBlock()->getWords().empty()) continue;
      if (++idx == selectCursor_) {
        cursorY = probe.yPos;
        break;
      }
    }
  }
  if (cursorY >= 0) {
    if (cursorY < scrollY_) scrollY_ = cursorY;
    if (cursorY + lineH > scrollY_ + viewHeight) scrollY_ = cursorY + lineH - viewHeight;
    if (scrollY_ < 0) scrollY_ = 0;
  }

  const bool bandOn = selectAnchor_ >= 0;
  const int lo = bandOn ? std::min(selectAnchor_, selectCursor_) : 0;
  const int hi = bandOn ? std::max(selectAnchor_, selectCursor_) : -1;
  const int yOffset = viewTop - scrollY_;
  const int caretH = std::max(2, lineH - 4);
  int idx = -1;
  for (const auto& el : page_->elements) {
    const int sy = el->yPos + yOffset;
    switch (el->getTag()) {
      case TAG_PageLine: {
        const auto& line = static_cast<const PageLine&>(*el);
        // The same test extractPageLines() uses to skip a line, so this index
        // walks lineRefs_ 1:1 (the extraction loop stops rather than skipping
        // a line once the pool runs out).
        if (line.getBlock() == nullptr || line.getBlock()->getWords().empty()) break;
        ++idx;
        // There is no pixel clip path, so a line that does not fit the window
        // entirely is dropped instead of being allowed to overpaint the header
        // or the button hints.
        if (sy < viewTop || sy + lineH > viewBottom) break;
        const bool picked = bandOn && idx >= lo && idx <= hi;
        const bool isCursor = idx == selectCursor_;
        el->render(renderer, fontId, marginL, yOffset);
        if (picked) {
          drawSelectionRule(renderer, marginL + line.xPos, sy + lineH - 4, rightX - (marginL + line.xPos));
        }
        if (isCursor) {
          renderer.fillRect(caretX, sy + 2, 4, caretH, true);
        }
        break;
      }
      case TAG_PageHorizontalRule:
        if (sy >= viewTop && sy < viewBottom) el->render(renderer, fontId, marginL, yOffset);
        break;
      case TAG_PageImage:
      default:
        // Images are skipped deliberately: PageImage has no clip path (it would
        // paint outside the window), and every pass would re-decode it from SD.
        // The gap it leaves keeps the lines below it at their original
        // positions, so the layout still matches the reading page.
        break;
    }
  }
}

void AskAiActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int left = metrics.contentSidePadding;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_ASK_AI));

  const int y0 = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int textWidth = pageWidth - 2 * left;
  int y = y0;

  switch (state_) {
    case State::Picking: {
      // One editorial list instead of the generic menu: numbered rows, the
      // scope as a quiet trailing tag, and a single grey focus card. The rule
      // under each unselected row keeps nine rows legible without boxing them.
      const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
      const int sectionH = CompanionUi::drawSectionLabel(renderer, left, y, textWidth, tr(STR_COMPANION_ASK_SECTION));
      const int rowsTop = y + sectionH + metrics.verticalSpacing;
      const int rowsHeight = std::max(1, bottom - rowsTop);
      int rowH = rowsHeight / ROW_COUNT;
      rowH = std::min(52, std::max(30, rowH));
      const int rowsWidth = textWidth - metrics.sideButtonHintsWidth;
      for (int i = 0; i < ROW_COUNT; ++i) {
        const char* scope =
            I18N.get(kRowScope[i] == Scope::Chapter ? StrId::STR_ASK_SCOPE_CHAPTER : StrId::STR_ASK_SCOPE_PAGE);
        CompanionUi::drawSelectRow(renderer, Rect{left, rowsTop + i * rowH, rowsWidth, rowH - 3}, i + 1,
                                   I18N.get(kRowLabels[i]), scope, i == selectedIndex_);
      }
      break;
    }

    case State::Selecting: {
      // The question that got us here, then this page drawn in its reading
      // layout: the reader's own margins, line positions and per-word
      // placement — not a re-typeset copy. The band being picked shows as a
      // wavy rule under each line, and the moving end carries a caret in the
      // left gutter.
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, question_, textWidth, 2)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      y += metrics.verticalSpacing;
      const int viewTop = y;
      const int viewHeight = pageHeight - y - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
      const int minLineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
      if (page_ == nullptr || viewHeight < minLineHeight) {
        LOG_ERR("ASK", "selection view unusable (page=%d, view h=%d, line h=%d)", page_ != nullptr, viewHeight,
                minLineHeight);
        break;
      }
      if (!prewarmed_) {
        // The reader's two-pass pattern, run ONCE per selection instead of per
        // repaint: the scope clears the glyph cache on construction and again
        // on destruction, so a fresh scope every key press would re-prewarm the
        // whole page each time. Held, the first pass records the glyphs, the
        // prewarm rasterizes them, and every later pass is a cache hit.
        prewarmScope_.emplace(renderer.getFontCacheManager()->createPrewarmScope());
        renderSelectionView(viewTop, viewHeight);  // scan pass: records, paints nothing
        prewarmScope_->endScanAndPrewarm();
        prewarmed_ = true;
      }
      renderSelectionView(viewTop, viewHeight);
      break;
    }

    case State::Asking: {
      // Painted just before the blocking relay call; the e-ink panel holds it
      // until the reply settles. A large first fingerprint reports progress
      // here instead of leaving the reader staring at an unexplained wait.
      char status[96];
      if (relayRequest_ && relayRequest_->kind == RelayRequest::Kind::Ingest) {
        const StrId label = relayRequest_->uploading.load(std::memory_order_relaxed) ? StrId::STR_BOOK_UPLOADING
                                                                                     : StrId::STR_BOOK_INDEXING;
        std::snprintf(status, sizeof(status), "%s %u%%", I18N.get(label),
                      static_cast<unsigned>(relayRequest_->ingestProgress.load(std::memory_order_relaxed)));
      } else if (identifyingBook_) {
        std::snprintf(status, sizeof(status), "%s %u%%", tr(STR_AI_IDENTIFYING),
                      static_cast<unsigned>(identifyPercent_));
      } else {
        std::snprintf(status, sizeof(status), "%s", tr(STR_VOICE_RELAY_WAITING));
      }
      renderer.drawText(UI_10_FONT_ID, left, y, status, true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, question_, textWidth, 3)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      // The blocking relay call owns the panel until the reply settles; the
      // buddy over the empty half reads as "thinking", so the wait does not
      // look like a stalled screen.
      CompanionMascot::drawBlock(
          renderer, left, y + metrics.verticalSpacing, textWidth,
          pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - y - metrics.verticalSpacing,
          CompanionMascot::Pose::Think, nullptr);
      break;
    }

    case State::Answer: {
      // Question sits in a grey quotation card, answer below it under its own
      // eyebrow. The saved state is a pill on the question heading rather than
      // a second text run competing with the scroll read-out.
      const int savedChipW = saved_ ? renderer.getTextWidth(SMALL_FONT_ID, tr(STR_ASK_SAVED)) + 14 : 0;
      // Chapter discussion opens from a fixed invite, so echo the moment
      // ("this chapter, finished") instead of dumping the long relay prompt.
      // The open question itself belongs in the discussion panel below.
      CompanionUi::drawSectionLabel(
          renderer, left, y, textWidth - (savedChipW > 0 ? savedChipW + 10 : 0),
          chapterDiscussionMode_ ? tr(STR_COMPANION_DISCUSS_SECTION) : tr(STR_COMPANION_YOU_ASKED));
      if (saved_) {
        CompanionUi::drawChip(renderer, left + textWidth, y - 2, tr(STR_ASK_SAVED), false);
      }
      y += lineHeight + 4;
      const auto questionLines =
          renderer.wrappedText(UI_10_FONT_ID, chapterDiscussionMode_ ? tr(STR_AI_CHAPTER_INVITE) : question_,
                               textWidth - 20, chapterDiscussionMode_ ? 1 : 2);
      const int questionCardH = lineHeight * static_cast<int>(questionLines.size()) + 16;
      renderer.fillRoundedRect(left, y, textWidth, questionCardH, 6, Color::LightGray);
      int qy = y + 8;
      for (const auto& line : questionLines) {
        renderer.drawText(UI_10_FONT_ID, left + 10, qy, line.c_str());
        qy += lineHeight;
      }
      y += questionCardH + metrics.verticalSpacing;

      const int labelY = y;
      CompanionUi::drawSectionLabel(renderer, left, y, textWidth, tr(STR_COMPANION_BUDDY_SAYS));
      y += lineHeight + 4;
      if (uncited_) {
        for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, I18N.get(uncitedNotice_), textWidth, 3)) {
          renderer.drawText(UI_10_FONT_ID, left, y, line.c_str(), true, EpdFontFamily::ITALIC);
          y += lineHeight;
        }
        y += metrics.verticalSpacing;
      }
      // Chapter discussion reserves the bottom strip for the selectable open
      // questions, so the answer window ends above it.
      const int discussionPanelHeight = chapterDiscussionMode_ ? (lineHeight * 3 + 18) : 0;
      const int bottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - discussionPanelHeight;
      int maxLines = lineHeight > 0 ? (bottom - y) / lineHeight : 1;
      if (maxLines < 1) maxLines = 1;
      // Body text stays clear of the right margin, where the side Up/Down
      // scroll hints are drawn (same split as VoiceActivity's answer window).
      const int bodyWidth = textWidth - metrics.sideButtonHintsWidth;
      const size_t total = drawWindow(UI_10_FONT_ID, answerText(), left, y, bodyWidth, maxLines, answerScroll_);
      if (total > static_cast<size_t>(maxLines)) {
        const size_t lastShown = std::min(total, answerScroll_ + static_cast<size_t>(maxLines));
        char pos[24];
        snprintf(pos, sizeof(pos), "%u-%u/%u", static_cast<unsigned>(answerScroll_ + 1),
                 static_cast<unsigned>(lastShown), static_cast<unsigned>(total));
        renderer.drawText(UI_10_FONT_ID, pageWidth - left - renderer.getTextWidth(UI_10_FONT_ID, pos), labelY, pos);
      }
      if (chapterDiscussionMode_) {
        const int panelTop = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - discussionPanelHeight;
        // White-clear first: LightGray is a dither, so a grey fill alone lets
        // the answer text underneath show through the panel.
        renderer.fillRect(left, panelTop, textWidth, discussionPanelHeight, false);
        renderer.fillRoundedRect(left, panelTop, textWidth, discussionPanelHeight, 6, Color::LightGray);
        renderer.fillRect(left, panelTop + 8, 3, discussionPanelHeight - 16, true);
        const int panelX = left + 12;
        const int sectionH = CompanionUi::drawSectionLabel(renderer, panelX, panelTop + 8, textWidth - 24 - 70,
                                                           tr(STR_COMPANION_DISCUSS_SECTION));
        CompanionUi::drawStepDots(renderer, left + textWidth - 12, panelTop + 8 + (sectionH - 7) / 2,
                                  DISCUSSION_QUESTION_COUNT, discussionIndex_);
        int qy = panelTop + 8 + sectionH + 2;
        for (const auto& line :
             renderer.wrappedText(UI_10_FONT_ID, I18N.get(kDiscussionQuestions[discussionIndex_]), textWidth - 24, 2)) {
          renderer.drawText(UI_10_FONT_ID, panelX, qy, line.c_str(), true, EpdFontFamily::BOLD);
          qy += lineHeight;
        }
      }
      break;
    }

    case State::Citations: {
      int top = y0;
      if (citationError_ != nullptr) {
        for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, citationError_, textWidth, 2)) {
          renderer.drawText(UI_10_FONT_ID, left, top, line.c_str(), true, EpdFontFamily::ITALIC);
          top += lineHeight;
        }
        top += metrics.verticalSpacing;
      }
      top +=
          CompanionUi::drawSectionLabel(renderer, left, top, textWidth, tr(STR_AI_CITATIONS)) + metrics.verticalSpacing;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, tr(STR_AI_CITATION_HINT), textWidth, 2)) {
        renderer.drawText(UI_10_FONT_ID, left, top, line.c_str(), true, EpdFontFamily::ITALIC);
        top += lineHeight;
      }
      top += metrics.verticalSpacing;

      const int contentHeight = pageHeight - top - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
      if (uncited_ || queryResponse_ == nullptr || queryResponse_->citationCount == 0 || contentHeight <= 0) break;

      // One quote at a time, set as an editorial pull-quote rather than a
      // clipped list row: the sentence the answer rests on is the whole point
      // of this screen, so give it the width and the type size to be read.
      const int quoteLineH = renderer.getLineHeight(UI_12_FONT_ID);
      const int maxLines = std::max(1, (contentHeight - 46) / quoteLineH);
      const auto& citation = queryResponse_->citations[citationCursor_];
      const auto quoteLines = renderer.wrappedText(UI_12_FONT_ID, citation.quote, textWidth - 28, maxLines);
      const int cardHeight = std::min(contentHeight, static_cast<int>(quoteLines.size()) * quoteLineH + 44);

      renderer.fillRoundedRect(left, top, textWidth, cardHeight, 8, Color::LightGray);
      const int notchH = std::max(10, cardHeight - 18);
      renderer.fillRect(left, top + (cardHeight - notchH) / 2, 3, notchH, true);

      char number[8];
      snprintf(number, sizeof(number), "%02d", citationCursor_ + 1);
      renderer.drawText(UI_10_FONT_ID, left + 14, top + 10, number, true, EpdFontFamily::BOLD);
      if (queryResponse_->citationCount > 1) {
        char count[16];
        snprintf(count, sizeof(count), "/ %u", static_cast<unsigned>(queryResponse_->citationCount));
        renderer.drawText(SMALL_FONT_ID,
                          left + 14 + renderer.getTextWidth(UI_10_FONT_ID, number, EpdFontFamily::BOLD) + 4, top + 12,
                          count, true);
      }

      int quoteY = top + 32;
      for (const auto& line : quoteLines) {
        renderer.drawText(UI_12_FONT_ID, left + 14, quoteY, line.c_str(), true);
        quoteY += quoteLineH;
      }
      break;
    }

    case State::Failed: {
      renderer.drawText(UI_10_FONT_ID, left, y, failReason_, true, EpdFontFamily::BOLD);
      y += lineHeight + metrics.verticalSpacing;
      for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, question_, textWidth, 3)) {
        renderer.drawText(UI_10_FONT_ID, left, y, line.c_str());
        y += lineHeight;
      }
      CompanionMascot::drawBlock(
          renderer, left, y + metrics.verticalSpacing, textWidth,
          pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - y - metrics.verticalSpacing,
          CompanionMascot::Pose::Sad, nullptr);
      break;
    }
  }

  // Side Up/Down move the picker highlight and scroll the answer; the other
  // states never bind them, so their slots stay empty. The front Left/Right
  // keys mirror those two, so they carry the same labels on the same states.
  const bool navStates =
      state_ == State::Picking || state_ == State::Selecting || state_ == State::Answer || state_ == State::Citations;
  if (navStates && state_ != State::Selecting) {
    // Selecting keeps its nav labels (below) but drops the side chips: the
    // selection view spans the reading width, so the chips would sit on top of
    // the page text.
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  const char* confirmLabel = "";
  switch (state_) {
    case State::Picking:
      confirmLabel = tr(STR_SELECT);
      break;
    case State::Selecting:
      // First press marks the start, the second both closes the range and
      // sends it — the labels say which press is due.
      confirmLabel = selectAnchor_ < 0 ? tr(STR_ASK_RANGE_START) : tr(STR_ASK_RANGE_CONFIRM);
      break;
    case State::Answer:
      confirmLabel = chapterDiscussionMode_
                         ? tr(STR_AI_DISCUSS_SPEAK)
                         : ((queryResponse_ != nullptr && queryResponse_->citationCount > 0) ? tr(STR_AI_CITATIONS)
                                                                                             : tr(STR_ASK_AGAIN));
      break;
    case State::Citations:
      confirmLabel = tr(STR_AI_JUMP);
      break;
    case State::Failed:
      confirmLabel = tr(STR_RETRY);
      break;
    default:
      break;
  }
  const bool discussionChoices = state_ == State::Answer && chapterDiscussionMode_;
  const char* prevHint = discussionChoices ? tr(STR_DIR_LEFT) : (navStates ? tr(STR_DIR_UP) : "");
  const char* nextHint = discussionChoices ? tr(STR_DIR_RIGHT) : (navStates ? tr(STR_DIR_DOWN) : "");
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, prevHint, nextHint);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
