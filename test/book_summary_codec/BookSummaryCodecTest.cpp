#include <BookSummaryCodec.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

const char* kJson =
    R"({"book_id":"0123456789abcdef","scope":"chapter","spine_index":704,"title":"第一章","text":"山间的书店有一盏灯。\n这是第二段。","status":"ready","cache_key":"abc","generated_at":"2026-11-27T00:00:00Z","model":"qwen-flash","cached":false})";

TEST(BookSummaryCodecTest, ParsesSummary) {
  BookSummary::Response out;
  ASSERT_TRUE(BookSummary::parseResponse(kJson, std::strlen(kJson), "0123456789abcdef", out));
  EXPECT_STREQ(out.bookId, "0123456789abcdef");
  EXPECT_STREQ(out.title, "第一章");
  EXPECT_STREQ(out.text, "山间的书店有一盏灯。\n这是第二段。");
  EXPECT_EQ(out.spineIndex, 704);
  EXPECT_STREQ(out.cacheKey, "abc");
  EXPECT_FALSE(out.cached);
  EXPECT_TRUE(out.valid);
}

TEST(BookSummaryCodecTest, RejectsWrongBookAndEmptyText) {
  BookSummary::Response out;
  EXPECT_FALSE(BookSummary::parseResponse(kJson, std::strlen(kJson), "fedcba9876543210", out));
  const std::string empty = R"({"book_id":"0123456789abcdef","text":""})";
  EXPECT_FALSE(BookSummary::parseResponse(empty.c_str(), empty.size(), nullptr, out));
  const std::string missingId = R"({"text":"summary"})";
  EXPECT_FALSE(BookSummary::parseResponse(missingId.c_str(), missingId.size(), "0123456789abcdef", out));
}

TEST(BookSummaryCodecTest, TruncatesOnUtf8Boundary) {
  const std::string text = std::string(BookSummary::TEXT_CAP - 1, 'a') + "阅读";
  const std::string json = "{\"book_id\":\"0123456789abcdef\",\"text\":\"" + text + "\"}";
  BookSummary::Response out;
  ASSERT_TRUE(BookSummary::parseResponse(json.c_str(), json.size(), nullptr, out));
  EXPECT_LE(std::strlen(out.text), BookSummary::TEXT_CAP);
  EXPECT_EQ(std::string(out.text), std::string(std::strlen(out.text), 'a'));
}

TEST(BookSummaryCodecTest, StreamsLongUtf8Text) {
  std::string text;
  for (int i = 0; i < 300; ++i) text += "阅读摘要。";
  std::string json = "{\"book_id\":\"0123456789abcdef\",\"text\":\"" + text + "\",\"cached\":true}";
  BookSummary::Response out;
  ASSERT_TRUE(BookSummary::parseResponse(json.c_str(), json.size(), nullptr, out));
  EXPECT_EQ(std::string(out.text), text);
  EXPECT_TRUE(out.cached);
}

}  // namespace
