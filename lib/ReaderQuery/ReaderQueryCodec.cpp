#include "ReaderQueryCodec.h"

#include <StreamingJsonParser.h>

#include <cstdio>
#include <cstring>
#include <string_view>

namespace ReaderQuery {
namespace {

bool appendBytes(char* out, const size_t cap, size_t& pos, const char* text, const size_t len) {
  if (pos + len >= cap) return false;
  std::memcpy(out + pos, text, len);
  pos += len;
  out[pos] = '\0';
  return true;
}

bool appendLiteral(char* out, const size_t cap, size_t& pos, const char* text) {
  return appendBytes(out, cap, pos, text, std::strlen(text));
}

bool appendEscapedN(char* out, const size_t cap, size_t& pos, const char* text, const size_t len) {
  static constexpr char HEX[] = "0123456789abcdef";
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\\') {
      const char pair[2] = {'\\', static_cast<char>(c)};
      if (!appendBytes(out, cap, pos, pair, sizeof(pair))) return false;
    } else if (c < 0x20) {
      const char esc[6] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 0x0F]};
      if (!appendBytes(out, cap, pos, esc, sizeof(esc))) return false;
    } else {
      const char one = static_cast<char>(c);
      if (!appendBytes(out, cap, pos, &one, 1)) return false;
    }
  }
  return true;
}

bool appendEscaped(char* out, const size_t cap, size_t& pos, const char* text) {
  return appendEscapedN(out, cap, pos, text, std::strlen(text));
}

void copyCapped(char* dst, const size_t cap, const char* src, const size_t len) {
  if (cap == 0) return;
  size_t take = len < cap - 1 ? len : cap - 1;
  if (take < len) {
    while (take > 0 && (static_cast<unsigned char>(src[take]) & 0xC0) == 0x80) --take;
  }
  std::memcpy(dst, src, take);
  dst[take] = '\0';
}

uint16_t parseU16(const char* text, const size_t len) {
  uint32_t value = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') break;
    value = value * 10 + static_cast<uint32_t>(c - '0');
    if (value > 0xFFFF) return 0xFFFF;
  }
  return static_cast<uint16_t>(value);
}

Source parseSource(const char* text, const size_t len) {
  if (len == 4 && std::memcmp(text, "book", 4) == 0) return Source::Book;
  if (len == 8 && std::memcmp(text, "external", 8) == 0) return Source::External;
  return Source::Inference;
}

// One single-shot parse. The JSON schema is fixed (docs/ai-native/api-v1.md
// §4.1), so a depth + last-key state machine is enough and allocates nothing.
struct Parser {
  Response& out;
  int depth = 0;
  char key[40] = {};
  bool inSegments = false;
  bool inWarnings = false;
  bool inCitations = false;
  uint32_t segTextStart = 0;
  uint8_t segCiteStart = 0;
  uint8_t segCiteCount = 0;
  Source segSource = Source::Inference;
  Citation curCite{};

  void onKey(const char* value, const size_t len) { copyCapped(key, sizeof(key), value, len); }

  void appendText(const char* value, const size_t len) {
    if (len == 0) return;
    const size_t room = TEXT_CAP - 1 - out.textLen;
    size_t take = len < room ? len : room;
    // If the buffer fills mid-sequence, stop before this codepoint entirely;
    // never leave an incomplete UTF-8 sequence for the renderer.
    while (take > 0 && take < len && (static_cast<unsigned char>(value[take]) & 0xC0) == 0x80) --take;
    if (take < len) out.warnings |= WARN_TRUNCATED;
    if (take == 0) return;
    std::memcpy(out.text + out.textLen, value, take);
    out.textLen = static_cast<uint16_t>(out.textLen + take);
    out.text[out.textLen] = '\0';
  }

  void startSegment() {
    segTextStart = out.textLen;
    segCiteStart = out.citationCount;
    segCiteCount = 0;
    segSource = Source::Inference;
  }

  void endSegment() {
    const uint32_t len = out.textLen - segTextStart;
    if (len == 0) return;  // the relay drops empty segments before sending
    if (out.segmentCount >= MAX_SEGMENTS) {
      out.warnings |= WARN_TRUNCATED;
      return;
    }
    Segment& segment = out.segments[out.segmentCount++];
    segment.textOffset = segTextStart;
    segment.textLen = static_cast<uint16_t>(len > 0xFFFF ? 0xFFFF : len);
    segment.firstCitation = segCiteStart;
    segment.citationCount = segCiteCount;
    segment.source = segSource;
  }

  void startCitation() { curCite = Citation{}; }

  void endCitation() {
    if (curCite.passageId[0] == '\0' || curCite.quote[0] == '\0') return;
    if (out.citationCount >= MAX_CITATIONS || segCiteCount >= MAX_CITATIONS_PER_SEGMENT) {
      out.warnings |= WARN_TRUNCATED;
      return;
    }
    out.citations[out.citationCount++] = curCite;
    segCiteCount++;
  }

  void onWarning(const char* value, const size_t len) {
    if (len == 16 && std::memcmp(value, "CITATION_DROPPED", 16) == 0) {
      out.warnings |= WARN_CITATION_DROPPED;
    } else if (len == 21 && std::memcmp(value, "MODEL_OUTPUT_NOT_JSON", 21) == 0) {
      out.warnings |= WARN_MODEL_OUTPUT_NOT_JSON;
    } else if ((len == 17 && std::memcmp(value, "SEGMENT_TRUNCATED", 17) == 0) ||
               (len == 18 && std::memcmp(value, "SEGMENTS_TRUNCATED", 18) == 0)) {
      out.warnings |= WARN_TRUNCATED;
    }
  }

  void onString(const char* value, const size_t len) {
    if (inCitations && depth == 5) {
      if (std::strcmp(key, "passage_id") == 0) {
        copyCapped(curCite.passageId, sizeof(curCite.passageId), value, len);
      } else if (std::strcmp(key, "quote") == 0) {
        copyCapped(curCite.quote, sizeof(curCite.quote), value, len);
      } else if (std::strcmp(key, "spine_index") == 0) {
        curCite.spineIndex = parseU16(value, len);
      }
      return;
    }
    if (inSegments && depth == 3) {
      if (std::strcmp(key, "source") == 0) {
        segSource = parseSource(value, len);
      } else if (std::strcmp(key, "text") == 0) {
        appendText(value, len);
      }
      return;
    }
    if (inWarnings && depth == 2) {
      onWarning(value, len);
      return;
    }
    if (depth == 1) {
      if (std::strcmp(key, "request_id") == 0) {
        copyCapped(out.requestId, sizeof(out.requestId), value, len);
      } else if (std::strcmp(key, "book_id") == 0) {
        copyCapped(out.bookId, sizeof(out.bookId), value, len);
      }
    }
  }

  void onNumber(const char* value, const size_t len) {
    if (inCitations && depth == 5 && std::strcmp(key, "spine_index") == 0) {
      curCite.spineIndex = parseU16(value, len);
    }
  }

  void onValuePart(const char* value, const size_t len) {
    // The streaming parser chunks string values longer than its token buffer.
    // Only the two fields that can exceed it need chunk support.
    if (inCitations && depth == 5 && std::strcmp(key, "quote") == 0) {
      const size_t used = std::strlen(curCite.quote);
      copyCapped(curCite.quote + used, sizeof(curCite.quote) - used, value, len);
    } else if (inSegments && depth == 3 && std::strcmp(key, "text") == 0) {
      appendText(value, len);
    }
  }

  void onObjectStart() {
    depth++;
    if (depth == 3 && inSegments) startSegment();
    if (depth == 5 && inCitations) startCitation();
  }

  void onObjectEnd() {
    if (depth == 3 && inSegments) endSegment();
    if (depth == 5 && inCitations) endCitation();
    depth--;
  }

  void onArrayStart() {
    depth++;
    if (depth == 2) {
      if (std::strcmp(key, "segments") == 0) inSegments = true;
      if (std::strcmp(key, "warnings") == 0) inWarnings = true;
    } else if (depth == 4 && inSegments && std::strcmp(key, "citations") == 0) {
      inCitations = true;
    }
  }

  void onArrayEnd() {
    if (depth == 2) {
      inSegments = false;
      inWarnings = false;
    } else if (depth == 4) {
      inCitations = false;
    }
    depth--;
  }
};

void resetResponse(Response& out) {
  out.requestId[0] = '\0';
  out.bookId[0] = '\0';
  std::memset(out.text, 0, sizeof(out.text));
  for (auto& segment : out.segments) segment = Segment{};
  for (auto& citation : out.citations) citation = Citation{};
  out.textLen = 0;
  out.segmentCount = 0;
  out.citationCount = 0;
  out.warnings = WARN_NONE;
  out.valid = false;
}

void sOnKey(void* ctx, const char* key, size_t len) { static_cast<Parser*>(ctx)->onKey(key, len); }
void sOnString(void* ctx, const char* value, size_t len) { static_cast<Parser*>(ctx)->onString(value, len); }
void sOnNumber(void* ctx, const char* value, size_t len) { static_cast<Parser*>(ctx)->onNumber(value, len); }
void sOnBool(void*, bool) {}
void sOnNull(void*) {}
void sOnObjectStart(void* ctx) { static_cast<Parser*>(ctx)->onObjectStart(); }
void sOnObjectEnd(void* ctx) { static_cast<Parser*>(ctx)->onObjectEnd(); }
void sOnArrayStart(void* ctx) { static_cast<Parser*>(ctx)->onArrayStart(); }
void sOnArrayEnd(void* ctx) { static_cast<Parser*>(ctx)->onArrayEnd(); }
void sOnStringPart(void* ctx, const char* chunk, size_t len) { static_cast<Parser*>(ctx)->onValuePart(chunk, len); }

}  // namespace

size_t buildRequest(char* out, size_t cap, const Request& request) {
  if (out == nullptr || cap == 0 || request.requestId == nullptr || request.bookId == nullptr ||
      request.requestId[0] == '\0' || request.bookId[0] == '\0' || request.intent == nullptr ||
      request.contextScope == nullptr || request.language == nullptr) {
    return 0;
  }
  out[0] = '\0';
  size_t pos = 0;
  if (!appendLiteral(out, cap, pos, "{\"request_id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, request.requestId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"book_id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, request.bookId)) return 0;
  char number[64];
  std::snprintf(number, sizeof(number), "\",\"spine_index\":%d,\"passage_ids\":[", request.spineIndex);
  if (!appendLiteral(out, cap, pos, number)) return 0;

  const size_t idCount = request.passageIds == nullptr ? 0 : request.passageIdCount;
  bool wroteId = false;
  for (size_t i = 0; i < idCount && i < MAX_PASSAGE_IDS; ++i) {
    const char* id = request.passageIds[i];
    if (id == nullptr || id[0] == '\0') continue;
    if (wroteId && !appendLiteral(out, cap, pos, ",")) return 0;
    if (!appendLiteral(out, cap, pos, "\"")) return 0;
    if (!appendEscaped(out, cap, pos, id)) return 0;
    if (!appendLiteral(out, cap, pos, "\"")) return 0;
    wroteId = true;
  }

  if (!appendLiteral(out, cap, pos, "],\"selected_text\":\"")) return 0;
  if (request.selectedText != nullptr && !appendEscaped(out, cap, pos, request.selectedText)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"question\":\"")) return 0;
  if (request.question != nullptr && !appendEscaped(out, cap, pos, request.question)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"intent\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, request.intent)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"context_scope\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, request.contextScope)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"response_style\":\"concise\",\"language\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, request.language)) return 0;
  std::snprintf(number, sizeof(number), "\",\"max_output_tokens\":%u}", static_cast<unsigned>(request.maxOutputTokens));
  if (!appendLiteral(out, cap, pos, number)) return 0;
  return pos;
}

bool parseResponse(const char* json, size_t len, const char* expectedRequestId, Response& out) {
  resetResponse(out);
  if (json == nullptr || len == 0) return false;

  Parser parser{out};
  const JsonCallbacks callbacks = {&parser,        sOnKey,       sOnString,     sOnNumber,   sOnBool,      sOnNull,
                                   sOnObjectStart, sOnObjectEnd, sOnArrayStart, sOnArrayEnd, sOnStringPart};
  StreamingJsonParser stream(callbacks);
  stream.feed(json, len);
  if (stream.hasError()) return false;
  if (parser.depth != 0) return false;

  if (out.segmentCount == 0) return false;
  if (expectedRequestId != nullptr && expectedRequestId[0] != '\0' && out.requestId[0] != '\0' &&
      std::strcmp(expectedRequestId, out.requestId) != 0) {
    return false;
  }
  out.valid = true;
  return true;
}

size_t buildAiResultOp(char* out, const size_t cap, const char* opId, const char* entityId, const char* bookId,
                       const char* const* passageIds, const size_t passageCount, const char* question,
                       const uint64_t clientTs, const Response& response) {
  if (out == nullptr || cap == 0 || opId == nullptr || opId[0] == '\0' || entityId == nullptr || entityId[0] == '\0' ||
      bookId == nullptr || bookId[0] == '\0' || question == nullptr) {
    return 0;
  }
  size_t pos = 0;
  out[0] = '\0';
  char number[160];
  std::snprintf(number, sizeof(number), "{\"op_id\":\"");
  if (!appendLiteral(out, cap, pos, number)) return 0;
  if (!appendEscaped(out, cap, pos, opId)) return 0;
  std::snprintf(number, sizeof(number), "\",\"type\":\"ai_result.create\",\"client_ts\":%llu,\"book_id\":\"",
                static_cast<unsigned long long>(clientTs));
  if (!appendLiteral(out, cap, pos, number)) return 0;
  if (!appendEscaped(out, cap, pos, bookId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"payload\":{\"id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, entityId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"book_id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, bookId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"passage_ids\":[")) return 0;
  bool first = true;
  for (size_t i = 0; i < passageCount; ++i) {
    if (passageIds == nullptr || passageIds[i] == nullptr || passageIds[i][0] == '\0') continue;
    if (!first && !appendLiteral(out, cap, pos, ",")) return 0;
    if (!appendLiteral(out, cap, pos, "\"")) return 0;
    if (!appendEscaped(out, cap, pos, passageIds[i])) return 0;
    if (!appendLiteral(out, cap, pos, "\"")) return 0;
    first = false;
  }
  if (!appendLiteral(out, cap, pos, "],\"question\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, question)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"segments\":[")) return 0;

  bool firstSegment = true;
  for (uint8_t i = 0; i < response.segmentCount; ++i) {
    const Segment& segment = response.segments[i];
    if (segment.textOffset >= response.textLen || segment.textLen == 0) continue;
    size_t textLen = segment.textLen;
    if (segment.textOffset + textLen > response.textLen) textLen = response.textLen - segment.textOffset;

    if (!firstSegment && !appendLiteral(out, cap, pos, ",")) return 0;
    if (!appendLiteral(out, cap, pos, "{\"source\":\"")) return 0;
    if (!appendLiteral(out, cap, pos, sourceName(segment.source))) return 0;
    if (!appendLiteral(out, cap, pos, "\",\"text\":\"")) return 0;
    if (!appendEscapedN(out, cap, pos, response.text + segment.textOffset, textLen)) return 0;
    if (!appendLiteral(out, cap, pos, "\",\"citations\":[")) return 0;
    bool firstCitation = true;
    for (uint8_t c = 0; c < segment.citationCount; ++c) {
      const uint8_t index = static_cast<uint8_t>(segment.firstCitation + c);
      if (index >= response.citationCount) break;
      const Citation& citation = response.citations[index];
      if (citation.passageId[0] == '\0') continue;
      if (!firstCitation && !appendLiteral(out, cap, pos, ",")) return 0;
      if (!appendLiteral(out, cap, pos, "{\"passage_id\":\"")) return 0;
      if (!appendEscaped(out, cap, pos, citation.passageId)) return 0;
      std::snprintf(number, sizeof(number), "\",\"spine_index\":%u,\"quote\":\"",
                    static_cast<unsigned>(citation.spineIndex));
      if (!appendLiteral(out, cap, pos, number)) return 0;
      if (!appendEscaped(out, cap, pos, citation.quote)) return 0;
      if (!appendLiteral(out, cap, pos, "\"}")) return 0;
      firstCitation = false;
    }
    if (!appendLiteral(out, cap, pos, "]}")) return 0;
    firstSegment = false;
  }
  if (firstSegment) return 0;  // no text = nothing worth syncing
  if (!appendLiteral(out, cap, pos, "]}}")) return 0;
  return pos;
}

const char* sourceName(Source source) {
  switch (source) {
    case Source::Book:
      return "book";
    case Source::External:
      return "external";
    case Source::Inference:
    default:
      return "inference";
  }
}

}  // namespace ReaderQuery
