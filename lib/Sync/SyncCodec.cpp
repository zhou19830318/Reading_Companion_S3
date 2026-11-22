#include "SyncCodec.h"

#include <StreamingJsonParser.h>

#include <cstdio>
#include <cstring>

namespace Sync {
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

bool isContinuation(const unsigned char c) { return (c & 0xC0) == 0x80; }

// Returns the length of the valid UTF-8 sequence starting at p, or 0 when the
// bytes are invalid or truncated. A truncated sequence is common here: page
// excerpts are cut by byte budget, so the last code point may be partial. Raw
// invalid bytes would make the whole JSON body unparseable.
size_t utf8SequenceLength(const unsigned char* p) {
  if (p[0] < 0x80) return 1;
  if (p[0] >= 0xC2 && p[0] <= 0xDF) {
    return isContinuation(p[1]) ? 2 : 0;
  }
  if (p[0] == 0xE0) {
    return (p[1] >= 0xA0 && p[1] <= 0xBF && isContinuation(p[2])) ? 3 : 0;
  }
  if ((p[0] >= 0xE1 && p[0] <= 0xEC) || (p[0] >= 0xEE && p[0] <= 0xEF)) {
    return (isContinuation(p[1]) && isContinuation(p[2])) ? 3 : 0;
  }
  if (p[0] == 0xED) {
    return (p[1] >= 0x80 && p[1] <= 0x9F && isContinuation(p[2])) ? 3 : 0;
  }
  if (p[0] == 0xF0) {
    return (p[1] >= 0x90 && p[1] <= 0xBF && isContinuation(p[2]) && isContinuation(p[3])) ? 4 : 0;
  }
  if (p[0] >= 0xF1 && p[0] <= 0xF3) {
    return (isContinuation(p[1]) && isContinuation(p[2]) && isContinuation(p[3])) ? 4 : 0;
  }
  if (p[0] == 0xF4) {
    return (p[1] >= 0x80 && p[1] <= 0x8F && isContinuation(p[2]) && isContinuation(p[3])) ? 4 : 0;
  }
  return 0;
}

bool appendEscaped(char* out, const size_t cap, size_t& pos, const char* text) {
  static constexpr char HEX[] = "0123456789abcdef";
  static constexpr char REPLACEMENT[] = "\xEF\xBF\xBD";  // U+FFFD
  for (const char* p = text; *p != '\0';) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') {
      const char pair[2] = {'\\', static_cast<char>(c)};
      if (!appendBytes(out, cap, pos, pair, sizeof(pair))) return false;
      ++p;
    } else if (c < 0x20) {
      const char esc[6] = {'\\', 'u', '0', '0', HEX[c >> 4], HEX[c & 0x0F]};
      if (!appendBytes(out, cap, pos, esc, sizeof(esc))) return false;
      ++p;
    } else if (c < 0x80) {
      if (!appendBytes(out, cap, pos, p, 1)) return false;
      ++p;
    } else {
      const size_t len = utf8SequenceLength(reinterpret_cast<const unsigned char*>(p));
      if (len == 0) {
        if (!appendBytes(out, cap, pos, REPLACEMENT, sizeof(REPLACEMENT) - 1)) return false;
        ++p;
      } else {
        if (!appendBytes(out, cap, pos, p, len)) return false;
        p += len;
      }
    }
  }
  return true;
}

PushStatus parseStatus(const char* value, const size_t len) {
  if (len == 7 && std::memcmp(value, "applied", 7) == 0) return PushStatus::Applied;
  if (len == 9 && std::memcmp(value, "duplicate", 9) == 0) return PushStatus::Duplicate;
  if (len == 8 && std::memcmp(value, "rejected", 8) == 0) return PushStatus::Rejected;
  if (len == 8 && std::memcmp(value, "conflict", 8) == 0) return PushStatus::Conflict;
  return PushStatus::Unknown;
}

void copyCapped(char* dst, const size_t cap, const char* src, const size_t len) {
  if (cap == 0) return;
  const size_t take = len < cap - 1 ? len : cap - 1;
  std::memcpy(dst, src, take);
  dst[take] = '\0';
}

struct Parser {
  PushResult* results;
  size_t maxResults;
  size_t count = 0;
  int depth = 0;
  char key[32] = {};
  bool inResults = false;
  PushResult current;

  void onKey(const char* value, const size_t len) { copyCapped(key, sizeof(key), value, len); }

  void onString(const char* value, const size_t len) {
    if (!inResults || depth != 3) return;
    if (std::strcmp(key, "op_id") == 0) {
      copyCapped(current.opId, sizeof(current.opId), value, len);
    } else if (std::strcmp(key, "status") == 0) {
      current.status = parseStatus(value, len);
    }
  }

  void onObjectStart() {
    depth++;
    if (depth == 3 && inResults) current = PushResult{};
  }

  void onObjectEnd() {
    if (depth == 3 && inResults) {
      if (count < maxResults) {
        results[count++] = current;
      } else if (maxResults > 0) {
        // Keep the last result as a marker that the batch overflowed; the
        // expectedOps check below will refuse to clear the outbox.
        count = maxResults + 1;
      }
    }
    depth--;
  }

  void onArrayStart() {
    depth++;
    if (depth == 2 && std::strcmp(key, "results") == 0) inResults = true;
  }

  void onArrayEnd() {
    if (depth == 2) inResults = false;
    depth--;
  }
};

void sOnKey(void* ctx, const char* key, size_t len) { static_cast<Parser*>(ctx)->onKey(key, len); }
void sOnString(void* ctx, const char* value, size_t len) { static_cast<Parser*>(ctx)->onString(value, len); }
void sOnNumber(void*, const char*, size_t) {}
void sOnBool(void*, bool) {}
void sOnNull(void*) {}
void sOnObjectStart(void* ctx) { static_cast<Parser*>(ctx)->onObjectStart(); }
void sOnObjectEnd(void* ctx) { static_cast<Parser*>(ctx)->onObjectEnd(); }
void sOnArrayStart(void* ctx) { static_cast<Parser*>(ctx)->onArrayStart(); }
void sOnArrayEnd(void* ctx) { static_cast<Parser*>(ctx)->onArrayEnd(); }
void sOnStringPart(void*, const char*, size_t) {}

}  // namespace

size_t buildPushRequest(char* out, const size_t cap, const char* deviceId, const char* batchId, const char* const* ops,
                        const size_t count) {
  if (out == nullptr || cap == 0 || deviceId == nullptr || deviceId[0] == '\0' || batchId == nullptr ||
      batchId[0] == '\0' || ops == nullptr || count == 0 || count > MAX_RESULTS) {
    return 0;
  }
  size_t pos = 0;
  out[0] = '\0';
  if (!appendLiteral(out, cap, pos, "{\"device_id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, deviceId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"batch_id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, batchId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"operations\":[")) return 0;
  for (size_t i = 0; i < count; ++i) {
    if (ops[i] == nullptr || ops[i][0] == '\0') return 0;
    if (i > 0 && !appendLiteral(out, cap, pos, ",")) return 0;
    if (!appendLiteral(out, cap, pos, ops[i])) return 0;
  }
  if (!appendLiteral(out, cap, pos, "]}")) return 0;
  return pos;
}

size_t parsePushResponse(const char* json, const size_t len, PushResult* results, const size_t maxResults,
                         const size_t expectedOps, bool& allAcked) {
  allAcked = false;
  if (json == nullptr || len == 0 || results == nullptr || maxResults == 0) return 0;

  Parser parser;
  parser.results = results;
  parser.maxResults = maxResults;
  const JsonCallbacks callbacks = {&parser,        sOnKey,       sOnString,     sOnNumber,   sOnBool,      sOnNull,
                                   sOnObjectStart, sOnObjectEnd, sOnArrayStart, sOnArrayEnd, sOnStringPart};
  StreamingJsonParser stream(callbacks);
  stream.feed(json, len);
  if (stream.hasError() || parser.depth != 0 || parser.count == 0 || parser.count > maxResults) return 0;

  bool all = parser.count == expectedOps;
  for (size_t i = 0; i < parser.count; ++i) {
    if (results[i].status != PushStatus::Applied && results[i].status != PushStatus::Duplicate) all = false;
  }
  allAcked = all;
  return parser.count;
}

size_t buildNoteCreateOp(char* out, const size_t cap, const char* opId, const char* entityId, const char* bookId,
                         const char* passageId, const char* quote, const char* note, const char* kind,
                         const uint16_t spineIndex, const uint16_t pageNumber, const uint16_t pageCount,
                         const uint64_t clientTs) {
  if (out == nullptr || cap == 0 || opId == nullptr || opId[0] == '\0' || entityId == nullptr || entityId[0] == '\0' ||
      bookId == nullptr || bookId[0] == '\0' || note == nullptr) {
    return 0;
  }
  const char* safePassage = passageId != nullptr ? passageId : "";
  const char* safeQuote = quote != nullptr ? quote : "";
  const char* safeKind = (kind != nullptr && kind[0] != '\0') ? kind : "note";

  size_t pos = 0;
  out[0] = '\0';
  char number[192];
  if (!appendLiteral(out, cap, pos, "{\"op_id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, opId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"type\":\"note.create\",\"client_ts\":")) return 0;
  std::snprintf(number, sizeof(number), "%llu", static_cast<unsigned long long>(clientTs));
  if (!appendLiteral(out, cap, pos, number)) return 0;
  if (!appendLiteral(out, cap, pos, ",\"book_id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, bookId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"payload\":{\"id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, entityId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"book_id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, bookId)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"passage_id\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, safePassage)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"quote\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, safeQuote)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"note\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, note)) return 0;
  if (!appendLiteral(out, cap, pos, "\",\"kind\":\"")) return 0;
  if (!appendEscaped(out, cap, pos, safeKind)) return 0;
  std::snprintf(number, sizeof(number), "\",\"anchor\":{\"spine_index\":%u,\"page_number\":%u,\"page_count\":%u}}}",
                static_cast<unsigned>(spineIndex), static_cast<unsigned>(pageNumber), static_cast<unsigned>(pageCount));
  if (!appendLiteral(out, cap, pos, number)) return 0;
  return pos;
}

const char* pushStatusName(const PushStatus status) {
  switch (status) {
    case PushStatus::Applied:
      return "applied";
    case PushStatus::Duplicate:
      return "duplicate";
    case PushStatus::Rejected:
      return "rejected";
    case PushStatus::Conflict:
      return "conflict";
    case PushStatus::Unknown:
    default:
      return "unknown";
  }
}

}  // namespace Sync
