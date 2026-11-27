#pragma once

#include <cstddef>
#include <cstdint>

namespace BookSummary {

constexpr size_t BOOK_ID_CAP = 24;
constexpr size_t TITLE_CAP = 256;
constexpr size_t TEXT_CAP = 8192;
constexpr size_t CACHE_KEY_CAP = 65;
constexpr size_t MODEL_CAP = 64;
constexpr size_t GENERATED_AT_CAP = 32;

struct Response {
  char bookId[BOOK_ID_CAP] = {};
  char title[TITLE_CAP] = {};
  char text[TEXT_CAP + 1] = {};
  char cacheKey[CACHE_KEY_CAP] = {};
  char model[MODEL_CAP] = {};
  char generatedAt[GENERATED_AT_CAP] = {};
  int32_t spineIndex = -1;
  bool cached = false;
  bool valid = false;
};

bool parseResponse(const char* json, size_t len, const char* expectedBookId, Response& out);

}  // namespace BookSummary
