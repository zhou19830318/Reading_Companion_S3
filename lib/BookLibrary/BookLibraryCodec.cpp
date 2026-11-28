#include "BookLibraryCodec.h"

#include <StreamingJsonParser.h>

#include <cstdlib>
#include <cstring>

namespace BookLibrary {
namespace {

void copyCapped(char* dst, size_t cap, const char* src, size_t len) {
  if (cap == 0) return;
  const size_t take = len < cap - 1 ? len : cap - 1;
  std::memcpy(dst, src, take);
  dst[take] = '\0';
}

Status parseStatus(const char* value, size_t len) {
  if (len == 7 && std::memcmp(value, "pending", 7) == 0) return Status::Pending;
  if (len == 7 && std::memcmp(value, "parsing", 7) == 0) return Status::Parsing;
  if (len == 8 && std::memcmp(value, "indexing", 8) == 0) return Status::Indexing;
  if (len == 5 && std::memcmp(value, "ready", 5) == 0) return Status::Ready;
  if (len == 6 && std::memcmp(value, "failed", 6) == 0) return Status::Failed;
  return Status::Unknown;
}

struct Parser {
  IngestResponse& out;
  char key[32] = {};
  int depth = 0;

  void onKey(const char* value, size_t len) { copyCapped(key, sizeof(key), value, len); }
  void onValue(const char* value, size_t len) {
    if (depth != 1) return;
    if (std::strcmp(key, "book_id") == 0) {
      copyCapped(out.bookId, sizeof(out.bookId), value, len);
    } else if (std::strcmp(key, "sha256") == 0) {
      copyCapped(out.sha256, sizeof(out.sha256), value, len);
    } else if (std::strcmp(key, "status") == 0) {
      out.status = parseStatus(value, len);
    }
  }
  void onNumber(const char* value, const size_t) {
    if (depth != 1 || std::strcmp(key, "progress") != 0) return;
    const double progress = std::strtod(value, nullptr);
    out.progress = static_cast<int>(progress <= 1.0 ? progress * 100.0 : progress);
  }
  void onBool(bool value) {
    if (depth == 1 && std::strcmp(key, "duplicate") == 0) out.duplicate = value;
  }
  void onObjectStart() { ++depth; }
  void onObjectEnd() { --depth; }
};

void sOnKey(void* ctx, const char* key, size_t len) { static_cast<Parser*>(ctx)->onKey(key, len); }
void sOnString(void* ctx, const char* value, size_t len) { static_cast<Parser*>(ctx)->onValue(value, len); }
void sOnNumber(void* ctx, const char* value, size_t len) { static_cast<Parser*>(ctx)->onNumber(value, len); }
void sOnBool(void* ctx, bool value) { static_cast<Parser*>(ctx)->onBool(value); }
void sOnNull(void*) {}
void sOnObjectStart(void* ctx) { static_cast<Parser*>(ctx)->onObjectStart(); }
void sOnObjectEnd(void* ctx) { static_cast<Parser*>(ctx)->onObjectEnd(); }
void sOnArrayStart(void*) {}
void sOnArrayEnd(void*) {}
void sOnStringPart(void* ctx, const char* chunk, size_t len) { static_cast<Parser*>(ctx)->onValue(chunk, len); }

}  // namespace

bool parseIngestResponse(const char* json, const size_t len, const char* const expectedBookId, IngestResponse& out) {
  out = IngestResponse{};
  if (json == nullptr || len == 0) return false;
  Parser parser{out};
  const JsonCallbacks callbacks = {&parser,        sOnKey,       sOnString,     sOnNumber,   sOnBool,      sOnNull,
                                   sOnObjectStart, sOnObjectEnd, sOnArrayStart, sOnArrayEnd, sOnStringPart};
  StreamingJsonParser stream(callbacks);
  stream.feed(json, len);
  if (stream.hasError() || parser.depth != 0 || out.status == Status::Unknown || out.bookId[0] == '\0') return false;
  if (expectedBookId != nullptr && expectedBookId[0] != '\0' && std::strcmp(expectedBookId, out.bookId) != 0)
    return false;
  if (out.progress < 0) out.progress = 0;
  if (out.progress > 100) out.progress = 100;
  return true;
}

}  // namespace BookLibrary
