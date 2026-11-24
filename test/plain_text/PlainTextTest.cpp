#include <PlainText.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

std::string strip(const std::string& input) {
  std::string buf = input;
  buf.push_back('\0');  // writable terminator, like the activity buffers
  const size_t len = PlainText::stripMarkdown(buf.data(), input.size());
  EXPECT_EQ(buf[len], '\0');
  EXPECT_EQ(len, strlen(buf.c_str()));
  return buf.substr(0, len);
}

}  // namespace

TEST(PlainText, StripsEmphasisAndCodeMarkers) {
  EXPECT_EQ(strip("**8:00** 起床"), "8:00 起床");
  EXPECT_EQ(strip("*斜体* 与 _em_"), "斜体 与 em");
  EXPECT_EQ(strip("用 `snprintf` 构造"), "用 snprintf 构造");
  EXPECT_EQ(strip("~~删除~~ 与 ***粗斜***"), "删除 与 粗斜");
}

TEST(PlainText, KeepsProseStarsAndUnderscores) {
  EXPECT_EQ(strip("2 * 3 * 4 = 24"), "2 * 3 * 4 = 24");
  EXPECT_EQ(strip("改名 file_name_here 即可"), "改名 file_name_here 即可");
  EXPECT_EQ(strip("5*3"), "5*3");
}

TEST(PlainText, DropsHeadingMarkersAndFences) {
  EXPECT_EQ(strip("## 今日计划\n读书 30 分钟"), "今日计划\n读书 30 分钟");
  EXPECT_EQ(strip("```cpp\nint x = 1;\n```"), "int x = 1;");
  EXPECT_EQ(strip("#没空格"), "#没空格");
}

TEST(PlainText, CollapsesRunsOfBlankLines) {
  EXPECT_EQ(strip("a\n\n\n\nb"), "a\n\nb");
  EXPECT_EQ(strip("\n\na"), "a");
  EXPECT_EQ(strip("a\n\n"), "a");
}

TEST(PlainText, KeepsUtf8AndLineBreaks) {
  const std::string text = "第一行。\n第二行,**重点**。";
  EXPECT_EQ(strip(text), "第一行。\n第二行,重点。");
}

TEST(PlainText, IsIdempotentOnPlainText) {
  const std::string text = "今天读了 26 分钟。\n21:00-21:30 继续。";
  EXPECT_EQ(strip(text), text);
  EXPECT_EQ(strip(strip(text)), text);
}

TEST(PlainText, HandlesEmptyAndUnterminated) {
  EXPECT_EQ(strip(""), "");
  EXPECT_EQ(strip("**"), "");
  EXPECT_EQ(strip("**a"), "a");
}

TEST(PlainText, LeavesLinksAndQuotesAlone) {
  EXPECT_EQ(strip("见 [说明](https://example.com) 与 > 引用"), "见 [说明](https://example.com) 与 > 引用");
}

TEST(PlainText, StripsCrLfAndTrailingSpaces) { EXPECT_EQ(strip("a  \r\nb\t\r\n"), "a\nb"); }
