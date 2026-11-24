#include <ReadingSessionFormat.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

TEST(ReadingSessionFormat, EscapesBookPath) {
  char out[256];
  const size_t n =
      ReadingHistory::formatSessionLine(out, sizeof(out), "a\\b\"c", 3, 4, 2, 10, 1700000000, 1700000100, 48 + 8);
  ASSERT_GT(n, 0u);
  std::string s(out);
  EXPECT_NE(s.find("\\\\"), std::string::npos);  // backslash escaped
  EXPECT_NE(s.find("\\\""), std::string::npos);  // quote escaped
  EXPECT_NE(s.find("\"schema_version\":1"), std::string::npos);
}

TEST(ReadingSessionFormat, TimestampIncludesOffset) {
  char out[256];
  const size_t n =
      ReadingHistory::formatSessionLine(out, sizeof(out), "book.epub", 1, 2, 1, 5, 1700000000, 1700000005, 48 + 8);
  ASSERT_GT(n, 0u);
  std::string s(out);
  EXPECT_NE(s.find("+02:00"), std::string::npos);
}

TEST(ReadingSessionFormat, TruncatesOnSmallBuffer) {
  char out[32];
  const size_t n =
      ReadingHistory::formatSessionLine(out, sizeof(out), "book.epub", 1, 2, 1, 5, 1700000000, 1700000005, 48);
  EXPECT_EQ(n, 0u);
}

TEST(ReadingSessionFormat, FormatsPositionDurationAndPageCount) {
  char out[512];
  const size_t n = ReadingHistory::formatSessionLine(out, sizeof(out), "/Books/山间书店.epub", 3, 7, 4, 95, 1700000000,
                                                     1700000095, 48);
  ASSERT_GT(n, 0u);
  const std::string s(out);
  EXPECT_NE(s.find("\"position\":{\"spine\":3,\"page\":7}"), std::string::npos);
  EXPECT_NE(s.find("\"duration_sec\":95"), std::string::npos);
  EXPECT_NE(s.find("\"pages_advanced\":4"), std::string::npos);
  EXPECT_EQ(s.back(), '\n');
}

TEST(ReadingSessionFormat, EmptyBookPathIsAllowed) {
  char out[256];
  const size_t n = ReadingHistory::formatSessionLine(out, sizeof(out), "", 0, 0, 0, 1, 1700000000, 1700000001, 48);
  ASSERT_GT(n, 0u);
  EXPECT_NE(std::string(out).find("\"book_id\":\"\""), std::string::npos);
}

TEST(ReadingSessionFormat, RejectsUnsyncedClock) {
  char out[256];
  const size_t n =
      ReadingHistory::formatSessionLine(out, sizeof(out), "book.epub", 0, 0, 0, 1, 1600000000 - 1, 1600000000, 48);
  EXPECT_EQ(n, 0u);
}
