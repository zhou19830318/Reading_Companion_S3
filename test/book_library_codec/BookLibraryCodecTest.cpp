#include <gtest/gtest.h>

#include <cstring>

#include "BookLibraryCodec.h"

namespace {

using BookLibrary::IngestResponse;
using BookLibrary::Status;

TEST(BookLibraryCodec, ParsesUploadReady) {
  IngestResponse out;
  const char* json = R"({"book_id":"0123456789abcdef","sha256":"abc","status":"ready","duplicate":false})";
  ASSERT_TRUE(BookLibrary::parseIngestResponse(json, std::strlen(json), "0123456789abcdef", out));
  EXPECT_EQ(out.status, Status::Ready);
  EXPECT_STREQ(out.bookId, "0123456789abcdef");
  EXPECT_STREQ(out.sha256, "abc");
  EXPECT_FALSE(out.duplicate);
}

TEST(BookLibraryCodec, ParsesProcessingProgress) {
  IngestResponse out;
  const char* json = R"({"book_id":"0123456789abcdef","status":"indexing","progress":42,"error":null})";
  ASSERT_TRUE(BookLibrary::parseIngestResponse(json, std::strlen(json), "0123456789abcdef", out));
  EXPECT_EQ(out.status, Status::Indexing);
  EXPECT_EQ(out.progress, 42);
}

TEST(BookLibraryCodec, ParsesFractionalProcessingProgress) {
  IngestResponse out;
  const char* json = R"({"book_id":"0123456789abcdef","status":"parsing","progress":0.42})";
  ASSERT_TRUE(BookLibrary::parseIngestResponse(json, std::strlen(json), "0123456789abcdef", out));
  EXPECT_EQ(out.status, Status::Parsing);
  EXPECT_EQ(out.progress, 42);
}

TEST(BookLibraryCodec, ParsesDuplicateAndClampsProgress) {
  IngestResponse out;
  const char* json = R"({"book_id":"0123456789abcdef","status":"parsing","progress":140,"duplicate":true})";
  ASSERT_TRUE(BookLibrary::parseIngestResponse(json, std::strlen(json), nullptr, out));
  EXPECT_EQ(out.status, Status::Parsing);
  EXPECT_EQ(out.progress, 100);
  EXPECT_TRUE(out.duplicate);
}

TEST(BookLibraryCodec, RejectsMalformedUnknownAndMismatchedResponses) {
  IngestResponse out;
  EXPECT_FALSE(BookLibrary::parseIngestResponse("not json", 8, nullptr, out));
  EXPECT_FALSE(BookLibrary::parseIngestResponse(R"({"book_id":"a","status":"mystery"})", 40, nullptr, out));
  const char* json = R"({"book_id":"fedcba9876543210","status":"ready"})";
  EXPECT_FALSE(BookLibrary::parseIngestResponse(json, std::strlen(json), "0123456789abcdef", out));
}

}  // namespace
