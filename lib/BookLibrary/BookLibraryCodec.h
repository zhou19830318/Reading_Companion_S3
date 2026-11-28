#pragma once

#include <cstddef>
#include <cstdint>

namespace BookLibrary {

constexpr size_t BOOK_ID_CAP = 24;
constexpr size_t SHA256_CAP = 65;

enum class Status : uint8_t { Pending, Parsing, Indexing, Ready, Failed, Unknown };

struct IngestResponse {
  char bookId[BOOK_ID_CAP] = {};
  char sha256[SHA256_CAP] = {};
  Status status = Status::Unknown;
  int progress = 0;
  bool duplicate = false;
};

bool parseIngestResponse(const char* json, size_t len, const char* expectedBookId, IngestResponse& out);

}  // namespace BookLibrary
