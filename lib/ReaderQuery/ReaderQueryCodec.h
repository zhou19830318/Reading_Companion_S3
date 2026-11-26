#pragma once

#include <cstddef>
#include <cstdint>

// Reader-query wire contract (docs/ai-native/api-v1.md §4.1): the pure-C++
// half of POST /api/v1/reader/query, so the host suite in
// test/reader_query_codec can exercise the exact bytes without an ESP32.
//
// The response is flattened at parse time: all segment text goes into one
// contiguous UTF-8 buffer, segments point into it, and citations live in one
// shared table. That keeps the model small and lets the answer screen keep
// rendering from a single text window (same paging code as /chat), while the
// source label and citation numbers still come from the server structure
// rather than from model-generated text.
namespace ReaderQuery {

constexpr size_t REQUEST_ID_CAP = 40;  // 32 hex + margin
constexpr size_t BOOK_ID_CAP = 24;     // 16 hex + margin
constexpr size_t PASSAGE_ID_CAP = 48;  // book:spine:kindNNNN
constexpr size_t MAX_PASSAGE_IDS = 8;  // server reader_max_passages
constexpr size_t TEXT_CAP = 12288;     // flattened segment text, UTF-8 bytes
constexpr size_t MAX_SEGMENTS = 12;
constexpr size_t MAX_CITATIONS = 24;
constexpr size_t MAX_CITATIONS_PER_SEGMENT = 8;
constexpr size_t QUOTE_CAP = 120;  // display-only quote excerpt

enum class Source : uint8_t {
  Book,
  External,
  Inference,
};

// The relay's warning tokens that change what the UI must say. Unknown
// warnings are ignored (forward-compatible).
enum Warning : uint8_t {
  WARN_NONE = 0,
  WARN_CITATION_DROPPED = 1u << 0,       // a book claim lost its evidence
  WARN_MODEL_OUTPUT_NOT_JSON = 1u << 1,  // raw fallback, no structure
  WARN_TRUNCATED = 1u << 2,              // our text buffer filled first
};

// HTTP error classes that change what the Ask-AI screen says. The relay's
// structured error body is authoritative; a bare 404 means the route itself
// is missing (a relay older than the library API).
enum class ErrorKind : uint8_t {
  Auth,                // 401 / UNAUTHORIZED / DEVICE_NOT_ALLOWED
  NotUploaded,         // BOOK_NOT_FOUND: the relay has no copy of this file
  Indexing,            // BOOK_NOT_READY: uploaded, still parsing/indexing
  PassageMissing,      // PASSAGE_NOT_FOUND: file/cache version mismatch
  LibraryUnavailable,  // route-level 404: relay has no library API
  Upstream,            // LLM_* / RATE_LIMITED / OVERLOADED / 5xx
  Other,
};

// Classifies one non-2xx response. `body` may be empty or null.
ErrorKind classifyHttpError(int status, const char* body);

struct Citation {
  char passageId[PASSAGE_ID_CAP] = {};
  char quote[QUOTE_CAP] = {};
  uint16_t spineIndex = 0;
};

struct Segment {
  uint32_t textOffset = 0;  // into Response::text
  uint16_t textLen = 0;
  uint8_t firstCitation = 0;  // into Response::citations
  uint8_t citationCount = 0;
  Source source = Source::Inference;
};

// ~16 KB with the caps above; the activity allocates it in PSRAM, never on
// the stack or the internal heap (the TLS-handshake rule).
struct Response {
  char requestId[REQUEST_ID_CAP] = {};
  char bookId[BOOK_ID_CAP] = {};
  char text[TEXT_CAP] = {};
  Segment segments[MAX_SEGMENTS] = {};
  Citation citations[MAX_CITATIONS] = {};
  uint16_t textLen = 0;
  uint8_t segmentCount = 0;
  uint8_t citationCount = 0;
  uint8_t warnings = WARN_NONE;
  bool valid = false;
};

struct Request {
  const char* requestId = nullptr;
  const char* bookId = nullptr;
  int spineIndex = 0;
  const char* const* passageIds = nullptr;
  size_t passageIdCount = 0;
  const char* selectedText = "";
  const char* question = "";
  const char* intent = "explain";
  const char* contextScope = "current_passage";
  const char* language = "zh";
  uint16_t maxOutputTokens = 512;
};

// Serializes one request body. Returns the byte length (excluding NUL) or 0
// when an argument is invalid or the body does not fit; a partial body is
// never written.
size_t buildRequest(char* out, size_t cap, const Request& request);

// Parses one response body. `expectedRequestId` may be nullptr; when set, an
// echoed id that differs marks the response invalid (a stale round trip).
// Returns out.valid. Never allocates; this is a single-shot parse over `json`.
bool parseResponse(const char* json, size_t len, const char* expectedRequestId, Response& out);

const char* sourceName(Source source);

// Serializes one `ai_result.create` sync operation from a parsed response
// (docs/ai-native/api-v1.md §5.2). The op object is what SyncOutbox stores
// after the "SYO1 " tag. Returns the byte length or 0 when it does not fit.
size_t buildAiResultOp(char* out, size_t cap, const char* opId, const char* entityId, const char* bookId,
                       const char* const* passageIds, size_t passageCount, const char* question, uint64_t clientTs,
                       const Response& response);

}  // namespace ReaderQuery
