#include "DictionaryActivity.h"

#include <CloudConfig.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <VoiceRelayChatClient.h>
#include <WiFi.h>
#include <WifiIdleManager.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/CompanionMascot.h"
#include "components/CompanionUi.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int LABEL_GAP = 8;      // label → inline content spacing
constexpr int LABEL_INDENT = 12;  // wrapped definitions hang under the label
constexpr int RULE_GAP = 10;      // hero rule spacing
constexpr int MAX_WRAP_LINES = 40;
constexpr char LABEL_END[] = "\xE3\x80\x91";                           // 】 — closes the 【...】 section marker
constexpr char HERO_LABEL[] = "\xE3\x80\x90\xE5\xAD\x97\xE3\x80\x91";  // 【字】

// ── stroke-order view (田字格) ────────────────────────────────────────────
// Payload points are quantized 0..255 against the y-up frame defined in
// relay/tools/build_stroke_data.py; stored top-down, so y/255 grows downward.
struct StrokeBox {
  int x = 0;
  int y = 0;
  int size = 0;
};

struct StrokePoint {
  int x = 0;
  int y = 0;
};

// Parses one "x,y" token, advancing p past it. Defensive: the payload is
// server-built, but a malformed or clipped reply must never run off the end.
bool parseStrokePoint(const char*& p, StrokePoint& out) {
  if (*p < '0' || *p > '9') return false;
  int v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  out.x = v;
  if (*p++ != ',') return false;
  if (*p < '0' || *p > '9') return false;
  v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  out.y = v;
  return out.x <= 255 && out.y <= 255;
}

// Stroke-order digit at the stroke's starting point, flush right of it like a
// printed 笔顺图. A white halo keeps it readable over later strokes.
void drawStrokeNumber(GfxRenderer& renderer, const StrokeBox& box, int fontId, StrokePoint at, int index) {
  char label[4];
  snprintf(label, sizeof(label), "%d", index);
  const int w = renderer.getTextWidth(fontId, label);
  const int h = renderer.getLineHeight(fontId);
  int lx = at.x + 4;
  if (lx + w + 2 > box.x + box.size) lx = box.x + box.size - w - 3;
  const int ly = std::min(std::max(at.y - h / 2, box.y + 1), box.y + box.size - h - 1);
  renderer.fillRect(lx - 2, ly - 1, w + 4, h + 2, false);
  renderer.drawText(fontId, lx, ly, label, true);
}

// One polyline per stroke, numbered at its start. Coordinates map 0..255 onto
// box.size - 1 so strokes touch the frame like on paper.
void drawStrokeMedians(GfxRenderer& renderer, const StrokeBox& box, const char* payload, int lineWidth, int fontId) {
  const char* p = payload;
  int index = 1;
  while (*p != '\0') {
    StrokePoint prev{};
    StrokePoint start{};
    bool haveStart = false;
    while (*p != '\0' && *p != '|') {
      StrokePoint pt;
      if (!parseStrokePoint(p, pt)) {
        while (*p != '\0' && *p != '|') ++p;  // skip a malformed stroke's tail
        break;
      }
      const int x = box.x + pt.x * (box.size - 1) / 255;
      const int y = box.y + pt.y * (box.size - 1) / 255;
      if (!haveStart) {
        start = {x, y};
        haveStart = true;
      } else if (x != prev.x || y != prev.y) {
        renderer.drawLine(prev.x, prev.y, x, y, lineWidth, true);
      }
      prev = {x, y};
      if (*p == ';') ++p;
    }
    if (haveStart) drawStrokeNumber(renderer, box, fontId, start, index);
    ++index;
    if (*p == '|') ++p;
  }
}

// The 田字格: outer frame plus the dashed centre cross.
void drawHanziBox(GfxRenderer& renderer, const StrokeBox& box) {
  renderer.drawRect(box.x, box.y, box.size, box.size, 2, true);
  const int cx = box.x + box.size / 2;
  const int cy = box.y + box.size / 2;
  constexpr int DASH = 9;
  constexpr int GAP = 7;
  for (int y = box.y + 5; y + DASH <= box.y + box.size - 5; y += DASH + GAP) {
    renderer.drawLine(cx, y, cx, y + DASH, true);
  }
  for (int x = box.x + 5; x + DASH <= box.x + box.size - 5; x += DASH + GAP) {
    renderer.drawLine(x, cy, x + DASH, cy, true);
  }
}
}  // namespace

void DictionaryActivity::onEnter() {
  Activity::onEnter();
  // The lookup leaves the radio up on purpose; the idle manager powers it
  // down once this screen stops working. Every
  // lookup approach re-prompts for Wi-Fi when the radio has since gone, so the
  // teardown never strands the picker.
  WIFI_IDLE.arm();
  std::string ch;
  utf8AppendCodepoint(codepoint_, ch);
  strncpy(charBuf_, ch.c_str(), sizeof(charBuf_) - 1);
  heroChar_ = charBuf_;
  reply_ = makeUniqueNoThrow<char[]>(REPLY_CAP + 1);
  if (!reply_) {
    LOG_ERR("DICT", "OOM: reply buffer %u bytes", static_cast<unsigned>(REPLY_CAP + 1));
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  // Stroke view is optional: without the buffer the card simply offers no
  // stroke entry.
  strokes_ = makeUniqueNoThrow<char[]>(STROKES_CAP);
  if (strokes_) {
    strokes_[0] = '\0';
  } else {
    LOG_ERR("DICT", "OOM: stroke buffer %u bytes", static_cast<unsigned>(STROKES_CAP));
  }
  // Network first, then the request, then the reply: the entry point prompts
  // for WiFi before any waiting frame, so "查询中" is only ever painted once
  // the radio is up (or already was).
  if (WiFi.status() != WL_CONNECTED) {
    promptForWifi();
    return;
  }
  requestUpdate();
}

void DictionaryActivity::onExit() {
  voice_.stop();
  Activity::onExit();
}

bool DictionaryActivity::preventAutoSleep() { return state_ == State::Loading || voice_.busy(); }

void DictionaryActivity::loop() {
  voice_.tick();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    voice_.stop();
    // From the stroke view Back steps back to the card; only the card hands
    // the release edge to the picker below (a Back release there means "leave
    // the picker", so handing it an orphaned edge would close the mode).
    if (state_ == State::Strokes) {
      state_ = State::Card;
      requestUpdate();
      return;
    }
    finish();
    return;
  }

  if (state_ == State::Loading) {
    if (!started_) {
      started_ = true;
      // Paint the waiting frame before the blocking relay round (one question
      // at a time from the loop, the Ask-AI pattern).
      requestUpdateAndWait();
      beginLookup();
    }
    return;
  }

  if (state_ == State::Failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state_ = State::Loading;
      started_ = false;
      requestUpdate();
    }
    return;
  }

  // Strokes: a two-way toggle on the card — either button returns.
  if (state_ == State::Strokes) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state_ = State::Card;
      requestUpdate();
    }
    return;
  }

  // Card: Confirm opens the stroke view when the relay supplied a payload
  // (a character without data has none).
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && strokes_ && strokes_[0] != '\0') {
    state_ = State::Strokes;
    requestUpdate();
    return;
  }

  // Card: side Up/Down (and the mirrored front pair) scroll the entry.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (scrollLine_ > 0) {
      scrollLine_--;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (scrollLine_ < maxScroll_) {
      scrollLine_++;
      requestUpdate();
    }
    return;
  }
}

void DictionaryActivity::promptForWifi() {
  // Ask-at-send, matching Ask-AI: cancelling surfaces as a failed lookup whose
  // Confirm retry re-prompts — by then the radio usually reconnects from the
  // saved profile. `started_` stays set while the prompt is up so the Loading
  // branch does not fire a second prompt before this activity is pushed off.
  LOG_INF("DICT", "no WiFi; prompting for network");
  started_ = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             failText_ = tr(STR_STT_NO_NETWORK);
                             state_ = State::Failed;
                           } else {
                             // Re-enter the Loading path; the waiting frame is
                             // painted first, then the request goes out with
                             // the radio now up.
                             started_ = false;
                             state_ = State::Loading;
                           }
                           requestUpdate();
                         });
}

void DictionaryActivity::beginLookup() {
  if (!reply_) {
    // onEnter's OOM path left us without the reply buffer; retrying cannot fix
    // it and the relay writes into it.
    failText_ = tr(STR_MIC_OOM);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    // Safety net: the radio dropped between the entry check and the send.
    promptForWifi();
    return;
  }
  // A retry must not reuse the previous round's strokes.
  if (strokes_) strokes_[0] = '\0';

  CLOUD_CFG.load();  // NVS, idempotent; the same source the voice screen reads
  const bool relayConfigured = SETTINGS.voiceRelayEnabled != 0 && CLOUD_CFG.relayUrl[0] != '\0' &&
                               CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
  if (!relayConfigured) {
    LOG_ERR("DICT", "relay not configured");
    failText_ = tr(STR_VOICE_RELAY_NOT_CONFIGURED);
    state_ = State::Failed;
    requestUpdate();
    return;
  }

  bool truncated = false;
  VoiceRelay::ChatClient::Stats stats;
  const VoiceRelay::ChatClient::Result result = VoiceRelay::ChatClient::ask(
      CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId, charBuf_, reply_.get(), REPLY_CAP + 1,
      truncated, strokes_.get(), strokes_ ? STROKES_CAP : 0, "dict", &stats);
  if (result != VoiceRelay::ChatClient::Result::Ok) {
    LOG_ERR("DICT", "relay lookup failed: %s (http %d)", VoiceRelay::ChatClient::resultName(result), stats.httpStatus);
    failText_ = tr(STR_VOICE_RELAY_FAILED);
    state_ = State::Failed;
    requestUpdate();
    return;
  }
  if (truncated) {
    LOG_ERR("DICT", "reply truncated at %u bytes", static_cast<unsigned>(REPLY_CAP));
  }
  LOG_INF("DICT", "reply %u bytes (http %d)", static_cast<unsigned>(stats.responseBytes), stats.httpStatus);
  rowsBuilt_ = false;
  scrollLine_ = 0;
  state_ = State::Card;
  // The character first, then its card: hearing the glyph before the gloss is
  // the point of a spoken dictionary entry.
  voice_.start(charBuf_, nullptr, reply_.get());
  requestUpdate();
}

void DictionaryActivity::buildCard(const int fontId, const int textWidth) {
  rows_.clear();
  if (!reply_) return;

  const auto pushRow = [&](std::string label, const char* rest) {
    CardRow row;
    row.label = std::move(label);
    if (rest != nullptr && rest[0] != '\0') {
      // Labeled entries hang their wrapped text under the label, so the
      // definition column stays aligned instead of returning to the margin.
      const int rowWidth = row.label.empty() ? textWidth : textWidth - LABEL_INDENT;
      if (row.label.empty()) {
        row.content = renderer.wrappedText(fontId, rest, rowWidth, MAX_WRAP_LINES);
      } else {
        const int labelW = renderer.getTextWidth(fontId, row.label.c_str());
        const int restW = renderer.getTextWidth(fontId, rest);
        if (labelW + LABEL_GAP + restW <= rowWidth) {
          row.inlineContent = true;
          row.content.emplace_back(rest);
        } else {
          row.content = renderer.wrappedText(fontId, rest, rowWidth, MAX_WRAP_LINES);
        }
      }
    }
    if (!row.label.empty() || !row.content.empty()) rows_.push_back(std::move(row));
  };

  const char* p = reply_.get();
  while (*p != '\0') {
    const char* nl = strchr(p, '\n');
    size_t len = nl != nullptr ? static_cast<size_t>(nl - p) : strlen(p);
    size_t b = 0;
    while (b < len && (p[b] == ' ' || p[b] == '\t' || p[b] == '\r')) b++;
    while (len > b && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r')) len--;

    if (len > b) {
      std::string line(p + b, len - b);
      if (line.rfind("\xE3\x80\x90", 0) == 0) {  // starts with 【
        const size_t end = line.find(LABEL_END);
        if (end != std::string::npos) {
          const std::string label = line.substr(0, end + strlen(LABEL_END));
          const std::string rest = line.substr(end + strlen(LABEL_END));
          if (label == HERO_LABEL) {
            // The hero glyph is drawn separately above the card body.
            std::string hero = rest;
            const size_t hb = hero.find_first_not_of(" \t");
            if (hb != std::string::npos) heroChar_ = hero.substr(hb);
          } else {
            pushRow(label, rest.c_str());
          }
          if (nl == nullptr) break;
          p = nl + 1;
          continue;
        }
      }
      pushRow(std::string(), line.c_str());
    }
    if (nl == nullptr) break;
    p = nl + 1;
  }
}

void DictionaryActivity::renderStrokesView(const int left, const int right, const int contentTop,
                                           const int contentBottom) {
  // Legibility first: the square grows to the smaller of the content box's
  // dimensions and centres in the remainder (portrait is width-bound).
  StrokeBox box;
  box.size = std::min(right - left, contentBottom - contentTop);
  if (box.size < 16) return;
  box.x = left + ((right - left) - box.size) / 2;
  box.y = contentTop + ((contentBottom - contentTop) - box.size) / 2;
  drawHanziBox(renderer, box);
  const int lineWidth = std::max(3, box.size / 44);  // skeleton at ~168px pen-equivalent
  drawStrokeMedians(renderer, box, strokes_.get(), lineWidth, UI_10_FONT_ID);
}

void DictionaryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, true);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_DICTIONARY));

  // Same face as the Ask-AI and voice screens: the reader font, so CJK glyphs
  // come from the user's own SD font rather than a fallback-less builtin.
  const int bodyFont = SETTINGS.getReaderFontId();

  const int left = safe.x + metrics.contentSidePadding;
  const int right = safe.x + safe.width - metrics.contentSidePadding;
  const int textWidth = right - left;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = safe.y + safe.height - metrics.verticalSpacing;

  const int heroLineH = renderer.getLineHeight(bodyFont);
  const int bodyLineH = heroLineH;

  // Hero plate: one large character on a grey slip, like a dictionary headword
  // card. It is the only filled surface before the page's content, so the eye
  // lands on the character first and the entry second.
  const int heroPlateH = heroLineH + 24;
  const int heroGlyphW = renderer.getTextWidth(bodyFont, heroChar_.c_str(), EpdFontFamily::BOLD);
  const int heroPlateW = std::min(textWidth, std::max(heroLineH + 24, heroGlyphW + 24));
  const int heroPlateX = left + (textWidth - heroPlateW) / 2;
  const auto drawHero = [&]() {
    renderer.fillRoundedRect(heroPlateX, contentTop, heroPlateW, heroPlateH, 8, Color::LightGray);
    const int heroW = renderer.getTextWidth(bodyFont, heroChar_.c_str());
    renderer.drawText(bodyFont, left + (textWidth - heroW) / 2, contentTop + 12, heroChar_.c_str(), true,
                      EpdFontFamily::BOLD);
  };

  if (state_ == State::Strokes) {
    renderStrokesView(left, right, contentTop, contentBottom);
  } else if (state_ == State::Loading) {
    drawHero();
    // Painted once before the blocking relay round; the panel holds it.
    const char* line = tr(STR_DICT_LOOKING);
    const int msgW = renderer.getTextWidth(bodyFont, line);
    const int msgY = contentTop + heroPlateH + RULE_GAP * 2;
    renderer.drawText(bodyFont, left + (textWidth - msgW) / 2, msgY, line, true);
    CompanionMascot::drawBlock(renderer, left, msgY + bodyLineH, textWidth, contentBottom - msgY - bodyLineH,
                               CompanionMascot::Pose::Lookup, nullptr);
  } else {
    drawHero();
    const int bodyTop = contentTop + heroPlateH + 12;

    if (state_ == State::Failed) {
      // Failure is a callout, not a stray line: the retry hint in the bottom
      // bar belongs to a visible, bounded block.
      const int calloutH = bodyLineH * 2 + 24;
      renderer.fillRoundedRect(left, bodyTop, textWidth, calloutH, 6, Color::LightGray);
      const int failW = renderer.getTextWidth(bodyFont, tr(STR_DICT_FAILED), EpdFontFamily::BOLD);
      renderer.drawText(bodyFont, left + (textWidth - failW) / 2, bodyTop + 8, tr(STR_DICT_FAILED), true,
                        EpdFontFamily::BOLD);
      if (failText_ != nullptr) {
        const int detailW = renderer.getTextWidth(bodyFont, failText_);
        renderer.drawText(bodyFont, left + (textWidth - detailW) / 2, bodyTop + 8 + bodyLineH, failText_, true);
      }
      CompanionMascot::drawBlock(renderer, left, bodyTop + calloutH + metrics.verticalSpacing, textWidth,
                                 contentBottom - bodyTop - calloutH - metrics.verticalSpacing,
                                 CompanionMascot::Pose::Sad, nullptr);
    } else {
      // Card body stays clear of the right-hand side-button hints; the hero
      // plate above can use the full measure because those hints start lower.
      const int cardWidth = textWidth - metrics.sideButtonHintsWidth;
      if (!rowsBuilt_) {
        buildCard(bodyFont, cardWidth);
        rowsBuilt_ = true;
      }
      const int sectionH =
          CompanionUi::drawSectionLabel(renderer, left, bodyTop, cardWidth, tr(STR_COMPANION_DICT_SECTION));
      const int rowsTop = bodyTop + sectionH + 4;
      const auto rowLines = [](const CardRow& row) {
        if (row.content.empty()) return 1;
        return row.inlineContent ? 1 : 1 + static_cast<int>(row.content.size());
      };
      int totalLines = 0;
      for (const auto& row : rows_) totalLines += rowLines(row);

      const int visibleLines = bodyLineH > 0 ? (contentBottom - rowsTop) / bodyLineH : 0;
      maxScroll_ = std::max(0, totalLines - visibleLines);
      if (scrollLine_ > maxScroll_) scrollLine_ = maxScroll_;

      int y = rowsTop;
      int lineIdx = 0;
      for (const auto& row : rows_) {
        const int lines = rowLines(row);
        if (lineIdx + lines <= scrollLine_) {
          lineIdx += lines;
          continue;
        }
        if (y > contentBottom) break;
        const int labelW = row.label.empty() ? 0 : renderer.getTextWidth(bodyFont, row.label.c_str());
        if (!row.label.empty()) {
          renderer.drawText(bodyFont, left, y, row.label.c_str(), true, EpdFontFamily::BOLD);
        }
        if (row.inlineContent && !row.content.empty()) {
          renderer.drawText(bodyFont, left + labelW + LABEL_GAP, y, row.content[0].c_str(), true);
          y += bodyLineH;
        } else {
          if (!row.label.empty()) y += bodyLineH;
          const int contentX = row.label.empty() ? left : left + LABEL_INDENT;
          for (const auto& contentLine : row.content) {
            if (y > contentBottom) break;
            renderer.drawText(bodyFont, contentX, y, contentLine.c_str(), true);
            y += bodyLineH;
          }
        }
        lineIdx += lines;
      }
    }
  }

  if (state_ == State::Card) {
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  }
  const char* confirmLabel = "";
  if (state_ == State::Failed) {
    confirmLabel = tr(STR_RETRY);
  } else if (state_ == State::Card && strokes_ && strokes_[0] != '\0') {
    confirmLabel = tr(STR_DICT_STROKES);
  }
  const char* scrollUp = state_ == State::Card ? tr(STR_DIR_UP) : "";
  const char* scrollDown = state_ == State::Card ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, scrollUp, scrollDown);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
