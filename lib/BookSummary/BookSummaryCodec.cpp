#include "BookSummaryCodec.h"

#include <StreamingJsonParser.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BookSummary {
namespace {

void copyCapped(char* dst, size_t cap, const char* src, size_t len) {
  if (cap == 0) return;
  size_t take = len < cap - 1 ? len : cap - 1;
  if (take < len) {
    while (take > 0 && (static_cast<unsigned char>(src[take]) & 0xC0) == 0x80) --take;
    if (take < len && static_cast<unsigned char>(src[take]) >= 0xC0) --take;
  }
  std::memcpy(dst, src, take);
  dst[take] = '\0';
}

void appendText(char* dst, size_t cap, size_t& used, const char* src, size_t len) {
  if (len == 0 || used + 1 >= cap) return;
  size_t room = cap - 1 - used;
  size_t take = len < room ? len : room;
  if (take < len) {
    while (take > 0 && (static_cast<unsigned char>(src[take]) & 0xC0) == 0x80) --take;
    if (take < len && static_cast<unsigned char>(src[take]) >= 0xC0) --take;
  }
  if (take == 0) return;
  std::memcpy(dst + used, src, take);
  used += take;
  dst[used] = '\0';
}

struct Parser {
  Response& out;
  char key[40] = {};
  int depth = 0;
  size_t textLen = 0;

  void onKey(const char* value, size_t len) { copyCapped(key, sizeof(key), value, len); }
  void onString(const char* value, size_t len) { onValue(value, len); }
  void onValue(const char* value, size_t len) {
    if (depth != 1) return;
    if (std::strcmp(key, "book_id") == 0) {
      copyCapped(out.bookId, sizeof(out.bookId), value, len);
    } else if (std::strcmp(key, "title") == 0) {
      copyCapped(out.title, sizeof(out.title), value, len);
    } else if (std::strcmp(key, "text") == 0) {
      appendText(out.text, sizeof(out.text), textLen, value, len);
    } else if (std::strcmp(key, "cache_key") == 0) {
      copyCapped(out.cacheKey, sizeof(out.cacheKey), value, len);
    } else if (std::strcmp(key, "model") == 0) {
      copyCapped(out.model, sizeof(out.model), value, len);
    } else if (std::strcmp(key, "generated_at") == 0) {
      copyCapped(out.generatedAt, sizeof(out.generatedAt), value, len);
    }
  }
  void onNumber(const char* value, size_t) {
    if (depth == 1 && std::strcmp(key, "spine_index") == 0) {
      out.spineIndex = static_cast<int32_t>(std::strtol(value, nullptr, 10));
    }
  }
  void onBool(bool value) {
    if (depth == 1 && std::strcmp(key, "cached") == 0) out.cached = value;
  }
  void onObjectStart() { ++depth; }
  void onObjectEnd() { --depth; }
};

void sOnKey(void* ctx, const char* key, size_t len) { static_cast<Parser*>(ctx)->onKey(key, len); }
void sOnString(void* ctx, const char* value, size_t len) { static_cast<Parser*>(ctx)->onString(value, len); }
void sOnNumber(void* ctx, const char* value, size_t len) { static_cast<Parser*>(ctx)->onNumber(value, len); }
void sOnBool(void* ctx, bool value) { static_cast<Parser*>(ctx)->onBool(value); }
void sOnNull(void*) {}
void sOnObjectStart(void* ctx) { static_cast<Parser*>(ctx)->onObjectStart(); }
void sOnObjectEnd(void* ctx) { static_cast<Parser*>(ctx)->onObjectEnd(); }
void sOnArrayStart(void*) {}
void sOnArrayEnd(void*) {}
void sOnStringPart(void* ctx, const char* chunk, size_t len) { static_cast<Parser*>(ctx)->onValue(chunk, len); }

}  // namespace

bool parseResponse(const char* json, size_t len, const char* expectedBookId, Response& out) {
  out = Response{};
  if (json == nullptr || len == 0) return false;
  Parser parser{out};
  const JsonCallbacks callbacks = {&parser,        sOnKey,       sOnString,     sOnNumber,   sOnBool,      sOnNull,
                                   sOnObjectStart, sOnObjectEnd, sOnArrayStart, sOnArrayEnd, sOnStringPart};
  StreamingJsonParser stream(callbacks);
  stream.feed(json, len);
  if (stream.hasError() || parser.depth != 0 || out.text[0] == '\0') return false;
  if (expectedBookId != nullptr && expectedBookId[0] != '\0' && out.bookId[0] == '\0') return false;
  if (expectedBookId != nullptr && expectedBookId[0] != '\0' && std::strcmp(expectedBookId, out.bookId) != 0) {
    return false;
  }
  out.valid = true;
  return true;
}

}  // namespace BookSummary
