#pragma once

#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/PassageRef.h>
#include <FontCacheManager.h>
#include <I18n.h>
#include <NoteStore.h>
#include <ReaderQueryClient.h>
#include <VoiceRelayChatClient.h>
#include <VoiceRelayCodec.h>

#include <memory>
#include <optional>

#include "activities/Activity.h"
#include "components/AnswerVoice.h"

// AI-01 (阅读中快捷键 → 上下文提问): pick a reading question from a preset
// list, speak or type one (VoiceActivity's question-input mode), or select a
// line range on the current page. An indexed book goes to the relay's
// /api/v1/reader/query with the page/selection's passage ids, so the answer is
// rendered as source-labelled segments with verified citations that can be
// jumped back to; a book the relay does not know (SD-copied, not uploaded)
// degrades to /api/v1/chat with an explicit "no citations" notice. Every round
// is kept as a bookmark entry (NoteStore) so it can be re-read from the
// reader's note list. The relay is the only backend: an unconfigured relay or
// a failed round is a hard failure with its reason on screen, never a fallback
// to another model.
//
// Why this is its own lean activity instead of VoiceActivity:
//  - no mic, no STT, no 32 KB history pool: the big buffers (prompt, record,
//    note line, reader/query model) are allocated LAZILY in PSRAM — after the
//    waiting frame, and the record/line scratch only after the answer — so
//    nothing large sits on the internal heap across the TLS handshake's dip;
//  - no work on entry: the radio and the session are untouched until a
//    question is picked, so the picker and the selection screen stay usable
//    offline and the Wi-Fi detour happens at send time (the same rule as the
//    note-mode fix in VoiceActivity::startRecording);
//  - the passage context is resolved just before starting the relay worker (a
//    Section page read for Page, the whole chapter server-side for Chapter), or
//    copied up front into a PSRAM-forced line pool (Range), so the page decode
//    never overlaps the TLS dip with internal-heap allocations.
struct RelayRequest;

class AskAiActivity final : public Activity {
 public:
  AskAiActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity("AskAi", renderer, mappedInput) {}

  // Position context armed by the reader while it is alive (one-shot, same
  // shape as VoiceActivity::setNoteContext). The shared_ptr copy keeps the
  // book reachable for the context extraction in buildPrompt().
  void setBookContext(const std::shared_ptr<Epub>& book, const char* bookBase, uint16_t spineIndex, uint16_t pageNumber,
                      uint16_t pageCount);

  // V1-07: the reader's chapter-end bar uses this instead of opening the
  // generic preset picker. onEnter paints a waiting frame, then the first loop
  // tick sends a current_chapter question whose wording asks for the AI's own
  // reading of the chapter plus open discussion prompts.
  void startChapterDiscussion();

  void onEnter() override;
  void onExit() override;
  void onPause() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State : uint8_t {
    Picking,    // preset / voice question list
    Selecting,  // line-range picker for the Range presets
    Asking,     // relay round in flight on a worker (waiting frame is interactive)
    Answer,     // settled reply, scrollable window
    Citations,  // citation picker over a reader/query answer
    Failed,     // round trip died; Confirm retries the same question
  };

  // Context scope of a question: the current page only, a forward window from
  // it (chapter), or an explicit line range picked on this page.
  enum class Scope : uint8_t { Page, Chapter, Range };

  static constexpr int PRESET_COUNT = 8;
  static constexpr int ROW_COUNT = PRESET_COUNT + 1;  // + voice/keyboard row
  static constexpr int ROW_CUSTOM = PRESET_COUNT;
  // Chapter context reads forward from the current page, at most this many
  // pages: the relay prompt cap (1536 bytes) holds well under one page of CJK
  // text anyway, and a bounded loop keeps the SD/page decode off the watchdog.
  static constexpr int MAX_CONTEXT_PAGES = 3;
  // Chapter discussion appends the reader's spoken viewpoint to the chosen
  // question, so this must hold question + viewpoint, not just a preset label.
  static constexpr size_t QUESTION_CAP = 1024;
  // Question + labels + reply always fit, so the record is never cut mid
  // character before NoteStore::buildLine sees it.
  static constexpr size_t RECORD_CAP = VoiceRelay::CHAT_TEXT_CAP + QUESTION_CAP + 64;
  static constexpr int WRAP_MAX_LINES = 512;
  // Range selection: the page's lines are copied once into a PSRAM-forced
  // pool (held from pick time through the TLS dial — never internal DRAM),
  // addressed by uint16 offset/length pairs so the member array stays small.
  // 12 KB ≈ one dense CJK page; extraction stops cleanly if it fills.
  static constexpr size_t LINE_POOL_SIZE = 12288;
  static constexpr int MAX_PAGE_LINES = 160;
  // The relay accepts at most 8 passage IDs per reader/query request
  // (docs/ai-native/api-v1.md §4.1); keep the same cap locally.
  static constexpr int MAX_QUERY_PASSAGES = 8;
  // The relay contract caps selected_text at 4000 bytes; the same buffer backs
  // the saved quote so a selection is extracted once per range question.
  static constexpr size_t SELECTED_TEXT_CAP = 4000;
  // System-provided open questions for the chapter-end discussion. Left/Right
  // picks one; Confirm opens voice capture for the reader's own viewpoint.
  static constexpr int DISCUSSION_QUESTION_COUNT = 4;
  static constexpr StrId kDiscussionQuestions[DISCUSSION_QUESTION_COUNT] = {
      StrId::STR_AI_DISCUSS_Q1, StrId::STR_AI_DISCUSS_Q2, StrId::STR_AI_DISCUSS_Q3, StrId::STR_AI_DISCUSS_Q4};
  struct LineRef {
    uint16_t off;
    uint16_t len;
    EpubPassage::Ref passage;
    uint32_t charStart;
    uint32_t charEnd;
  };
  // One unique source passage touched by the question, with the byte range
  // covered by the selected lines (UTF-8 offsets into the relay's normalized
  // passage text). Built by collectQueryPassages().
  struct PassageSpan {
    EpubPassage::Ref ref;
    uint32_t charStart;
    uint32_t charEnd;
  };

  // Preset table: row label and its context scope. flash-resident constexpr.
  static constexpr StrId kRowLabels[ROW_COUNT] = {
      StrId::STR_Q_SUMMARIZE_PAGE,    StrId::STR_Q_KEY_POINTS,          StrId::STR_Q_EXPLAIN,
      StrId::STR_Q_CONNECT_LIFE,      StrId::STR_Q_CHAPTER_SUMMARY,     StrId::STR_Q_CHAPTER_BACKGROUND,
      StrId::STR_Q_EXPLAIN_SELECTION, StrId::STR_Q_TRANSLATE_SELECTION, StrId::STR_Q_CUSTOM};
  static constexpr Scope kRowScope[ROW_COUNT] = {Scope::Page,  Scope::Page,    Scope::Page,
                                                 Scope::Page,  Scope::Chapter, Scope::Chapter,
                                                 Scope::Range, Scope::Range,   Scope::Chapter};
  // Intent for the relay's reader/query contract (docs/ai-native/api-v1.md
  // §4.1). Presets map to the closest server intent; custom questions use
  // "custom" so the server does not rewrite the user's wording.
  static constexpr const char* kRowIntent[ROW_COUNT] = {"summarize", "explain",         "explain",
                                                        "apply",     "chapter_summary", "chapter_background",
                                                        "explain",   "translate",       "custom"};

  void beginAsk(const char* question, Scope scope, const char* intent = "explain");
  bool extractPageLines();
  void enterSelecting();
  void exitSelectionView();
  void renderSelectionView(int viewTop, int viewHeight);
  // The relay setting is complete (URL + token + device id). Checked per ask,
  // so flipping the setting applies to the next question.
  bool relayConfigured();
  // POST /api/v1/reader/query (indexed book) or /api/v1/chat (unindexed book:
  // uncited Q&A) on a worker task. The waiting frame stays interactive so Back
  // can cancel and leave while the bounded socket call unwinds.
  void beginRelayAsk();
  void beginReaderQuery();
  void beginChat(StrId notice);
  void handleRelayCompletion();
  void cancelRelayRequest();
  void composeQueryAnswer();
  void enqueueAiResult();
  // Chapter discussion: capture the reader's spoken viewpoint for the selected
  // system question, then ask the relay to respond to that viewpoint with the
  // chapter still as context.
  void startDiscussionVoice();
  void submitDiscussionViewpoint(const char* viewpoint);
  void openCitations();
  void jumpToCitation(int index);
  bool findCitationPage(const ReaderQuery::Citation& citation, uint16_t& page) const;
  const char* contextScopeName() const;
  const char* queryLanguage() const;
  const char* answerText() const;
  bool buildPrompt();
  void saveRecord();
  void fail(const char* reason);
  // Book identity and passage collection for the relay's citation contract.
  // The id is computed lazily (first ask) because hashing the whole file is a
  // multi-second SD read; it runs behind State::Asking with a progress read-out
  // and the result is cached in a sidecar. An empty id means the file could not
  // be identified, and the reply degrades to uncited Q&A with a notice saying
  // so (the relay decides separately whether the book is uploaded/indexed).
  void ensureBookId();
  static void hashProgress(void* ctx, uint8_t percent);
  void collectQueryPassages();
  void addPassageSpan(const EpubPassage::Ref ref, uint32_t charStart, uint32_t charEnd);
  bool ensureSelectedText();
  // V1.1: a confirmed range is a quote the reader chose to keep, not just
  // question context. Queue it for sync as a highlight before the AI round.
  void saveSelectionQuote();
  size_t drawWindow(int fontId, const char* text, int x, int y, int width, int capacity, size_t& firstLine);
  // Armed by the reader (see setBookContext).
  std::shared_ptr<Epub> book_;
  char bookBase_[Notes::NoteFormat::NAME_PATH_SIZE] = {};
  char bookId_[17] = {};  // 16 hex + NUL, empty when unknown
  uint16_t spineIndex_ = 0;
  uint16_t pageNumber_ = 0;
  uint16_t pageCount_ = 0;
  PassageSpan queryPassages_[MAX_QUERY_PASSAGES] = {};
  int queryPassageCount_ = 0;
  // Formatted passage IDs for the reader/query body. sizeof("16hex:5digits:li5")
  // is well under 40; the pointer array avoids copying at call time.
  char queryPassageIds_[MAX_QUERY_PASSAGES][40] = {};
  const char* queryPassageIdPtrs_[MAX_QUERY_PASSAGES] = {};
  const char* intent_ = "explain";
  bool uncited_ = false;  // /chat fallback: the answer carries no book citations
  // Why this round is uncited. A StrId (not a char*) so a language switch
  // cannot leave a dangling translation pointer behind.
  StrId uncitedNotice_ = StrId::STR_AI_UNCITED;
  // First fingerprint in flight behind State::Asking. Written by the loop task,
  // read by the render task, like identifyPercent_ below.
  volatile bool identifyingBook_ = false;
  volatile uint8_t identifyPercent_ = 0;

  State state_ = State::Picking;
  bool chapterDiscussionPending_ = false;
  bool chapterDiscussionMode_ = false;
  int selectedIndex_ = 0;
  Scope scope_ = Scope::Page;
  char question_[QUESTION_CAP] = {};
  char discussionInput_[QUESTION_CAP] = {};
  int discussionIndex_ = 0;
  size_t answerScroll_ = 0;
  int citationCursor_ = 0;
  const char* citationError_ = nullptr;
  bool saved_ = false;
  const char* failReason_ = nullptr;
  // Back press seen while current: the finish waits for its release edge so
  // the reader never consumes an orphaned Back release (see loop()).
  bool backArmed_ = false;
  // Big buffers, allocated on first use (buildPrompt / saveRecord) — both
  // happen only after the link is up, so none of them is resident on the
  // internal heap across the TLS dial. unique_ptr members: no onExit
  // bookkeeping, freed with the activity.
  std::unique_ptr<char[]> prompt_;
  std::unique_ptr<char[]> record_;
  std::unique_ptr<char[]> lineBuf_;
  // Relay answer scratch (CHAT_TEXT_CAP + 1), allocated on the first round
  // only. Freed with the activity like the buffers above.
  std::unique_ptr<char[]> relayReply_;
  // Reader/query model and composed answer. Both are PSRAM-only: the response
  // lives across the blocking LLM turn, the composed text backs the paging
  // window. queryResponse_ is a raw heap_caps allocation (custom deleter not
  // worth the unique_ptr ceremony for one member); freed in onExit().
  ReaderQuery::Response* queryResponse_ = nullptr;
  char* queryText_ = nullptr;
  char* querySelectedText_ = nullptr;
  // Spoken answer: owns its text copy, so relayReply_ stays free for retries.
  AnswerVoice voice_;
  // Range selection state (State::Selecting).
  char* linePool_ = nullptr;
  size_t linePoolCap_ = 0;
  int lineCount_ = 0;
  int selectCursor_ = 0;
  int selectAnchor_ = -1;  // -1: start not picked yet
  int selStart_ = 0;       // inclusive, valid once a range was confirmed
  int selEnd_ = 0;
  bool selectionQuoteSaved_ = false;  // one quote per confirmed range, retries included
  LineRef lineRefs_[MAX_PAGE_LINES] = {};
  // The page the selection view draws. Owned here from extraction until
  // exitSelectionView() — which runs on every path out of State::Selecting —
  // so it is never resident across the TLS dial (the class comment's rule) nor
  // while the preset list is up.
  std::unique_ptr<Page> page_;
  // The worker owns its request copy and output buffers. A shared reference lets
  // onExit cancel immediately without freeing memory the worker still uses.
  std::shared_ptr<RelayRequest> relayRequest_;
  uint32_t relayDeadlineMs_ = 0;
  // Sliding window over the page's viewport-relative Y coordinates: the view
  // scrolls only to keep selectCursor_'s line whole inside it.
  int scrollY_ = 0;
  // Font prewarm for the selection view, held for as long as the view is up.
  // The reader's PrewarmScope clears the glyph cache in BOTH its constructor
  // and its destructor, so a scope per repaint would clear and re-prewarm the
  // whole page on every key press. Held instead: one scan + prewarm, then every
  // repaint is a page-buffer hit and nothing touches the SD font path.
  std::optional<FontCacheManager::PrewarmScope> prewarmScope_;
  bool prewarmed_ = false;
};
