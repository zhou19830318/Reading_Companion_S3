#include <ReadingAggregateFormat.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

using namespace ReadingHistory;

TEST(ReadingAggregateFormat, HeatLevelBoundaries) {
  EXPECT_EQ(heatLevel(0), 0);
  EXPECT_EQ(heatLevel(1), 1);
  EXPECT_EQ(heatLevel(14 * 60), 1);
  EXPECT_EQ(heatLevel(15 * 60), 2);  // 15 min enters the middle band
  EXPECT_EQ(heatLevel(39 * 60 + 59), 2);
  EXPECT_EQ(heatLevel(40 * 60), 3);
  EXPECT_EQ(heatLevel(5 * 3600), 3);
}

TEST(ReadingAggregateFormat, ParsesHistorySessionLine) {
  const char* line =
      "{\"schema_version\":1,\"ts_start\":\"2026-11-22T15:30:00+08:00\","
      "\"ts_end\":\"2026-11-22T15:52:30+08:00\",\"book_id\":\"/Books/山间书店.epub\","
      "\"position\":{\"spine\":0,\"page\":4},\"duration_sec\":1350,\"pages_advanced\":12}";
  uint32_t duration = 0;
  uint32_t pages = 0;
  ASSERT_TRUE(jsonUintField(line, "\"duration_sec\"", duration));
  ASSERT_TRUE(jsonUintField(line, "\"pages_advanced\"", pages));
  EXPECT_EQ(duration, 1350u);
  EXPECT_EQ(pages, 12u);

  char book[BOOK_ID_CAP];
  ASSERT_TRUE(jsonEscapedStringField(line, "\"book_id\"", book, sizeof(book)));
  EXPECT_STREQ(book, "/Books/山间书店.epub");
}

TEST(ReadingAggregateFormat, ToleratesSpacesAndUnknownFields) {
  const char* line = "{ \"duration_sec\" : 90, \"future\": \"x\", \"pages_advanced\" : 2 }";
  uint32_t duration = 0;
  ASSERT_TRUE(jsonUintField(line, "\"duration_sec\"", duration));
  EXPECT_EQ(duration, 90u);
}

TEST(ReadingAggregateFormat, MissingOrMalformedFieldsFail) {
  uint32_t v = 7;
  EXPECT_FALSE(jsonUintField("{\"pages_advanced\":3}", "\"duration_sec\"", v));
  EXPECT_FALSE(jsonUintField("{\"duration_sec\":\"90\"}", "\"duration_sec\"", v));
  EXPECT_FALSE(jsonUintField("{\"duration_sec\":18446744073709551616}", "\"duration_sec\"", v));

  char book[BOOK_ID_CAP];
  EXPECT_FALSE(jsonEscapedStringField("{\"book_id\":42}", "\"book_id\"", book, sizeof(book)));
  // Unterminated value must not be accepted.
  EXPECT_FALSE(jsonEscapedStringField("{\"book_id\":\"abc}", "\"book_id\"", book, sizeof(book)));
}

TEST(ReadingAggregateFormat, EscapedStringKeepsEscapes) {
  const char* line = "{\"book_id\":\"/Books/a\\\"b\\\\c.epub\"}";
  char book[BOOK_ID_CAP];
  ASSERT_TRUE(jsonEscapedStringField(line, "\"book_id\"", book, sizeof(book)));
  EXPECT_STREQ(book, "/Books/a\\\"b\\\\c.epub");
}

TEST(ReadingAggregateFormat, EscapedStringRefusesOverflow) {
  std::string line = "{\"book_id\":\"";
  line += std::string(200, 'x');
  line += "\"}";
  char book[64];
  EXPECT_FALSE(jsonEscapedStringField(line.c_str(), "\"book_id\"", book, sizeof(book)));
}

TEST(ReadingAggregateFormat, AddSessionMergesBooks) {
  DayAggregate day;
  addSession(day, 120, 2, "/Books/a.epub");
  addSession(day, 300, 5, "/Books/a.epub");
  addSession(day, 60, 1, "/Books/b.epub");
  EXPECT_EQ(day.totalSec, 480u);
  EXPECT_EQ(day.pagesAdvanced, 8u);
  EXPECT_EQ(day.sessions, 3u);
  ASSERT_EQ(day.bookCount, 2u);
  EXPECT_STREQ(day.books[0].id, "/Books/a.epub");
  EXPECT_EQ(day.books[0].sec, 420u);
  EXPECT_EQ(day.books[1].sec, 60u);
  EXPECT_FALSE(day.booksTruncated);
}

TEST(ReadingAggregateFormat, AddSessionCapsBookShares) {
  DayAggregate day;
  for (size_t i = 0; i < MAX_BOOKS_PER_DAY; ++i) {
    const std::string id = "/Books/" + std::to_string(i) + ".epub";
    addSession(day, 10, 1, id.c_str());
  }
  EXPECT_EQ(day.bookCount, MAX_BOOKS_PER_DAY);
  EXPECT_FALSE(day.booksTruncated);

  addSession(day, 10, 1, "/Books/overflow.epub");
  EXPECT_EQ(day.bookCount, MAX_BOOKS_PER_DAY);
  EXPECT_TRUE(day.booksTruncated);
  // Totals stay exact even when a share is dropped.
  EXPECT_EQ(day.totalSec, 10u * (MAX_BOOKS_PER_DAY + 1));
  EXPECT_EQ(day.sessions, MAX_BOOKS_PER_DAY + 1);
}

TEST(ReadingAggregateFormat, AddSessionSkipsTooLongBookId) {
  DayAggregate day;
  const std::string longId = "/Books/" + std::string(BOOK_ID_CAP, 'x') + ".epub";
  addSession(day, 15, 1, longId.c_str());
  EXPECT_EQ(day.bookCount, 0u);
  EXPECT_TRUE(day.booksTruncated);
  EXPECT_EQ(day.totalSec, 15u);
}

TEST(ReadingAggregateFormat, DayLineRoundTrip) {
  DayAggregate day;
  day.dayIndex = static_cast<int32_t>(ChatLog::ChatHistoryFormat::daysFromCivil(2026, 11, 22));
  addSession(day, 1350, 12, "/Books/山间书店.epub");
  addSession(day, 60, 1, "/Books/other.epub");

  char line[DAY_LINE_CAP];
  const size_t len = formatDayLine(line, sizeof(line), day);
  ASSERT_GT(len, 0u);
  EXPECT_EQ(std::strchr(line, '\n'), nullptr);

  DayStat stat;
  ASSERT_TRUE(parseDayLineStat(line, stat));
  EXPECT_EQ(stat.dayIndex, day.dayIndex);
  EXPECT_EQ(stat.totalSec, 1410u);
  EXPECT_EQ(stat.pagesAdvanced, 13u);
  EXPECT_EQ(stat.sessions, 2u);
  EXPECT_TRUE(stat.present);
}

TEST(ReadingAggregateFormat, DayLineMarksTruncatedBooks) {
  DayAggregate day;
  day.dayIndex = static_cast<int32_t>(ChatLog::ChatHistoryFormat::daysFromCivil(2026, 1, 2));
  day.booksTruncated = true;
  addSession(day, 30, 1, "/Books/a.epub");
  char line[DAY_LINE_CAP];
  ASSERT_GT(formatDayLine(line, sizeof(line), day), 0u);
  EXPECT_NE(std::strstr(line, "\"books_truncated\":true"), nullptr);
}

TEST(ReadingAggregateFormat, RejectsLineWithoutDate) {
  DayStat stat;
  EXPECT_FALSE(parseDayLineStat("{\"total_sec\":120}", stat));
  EXPECT_FALSE(parseDayLineStat("{\"date\":\"not-a-date\"}", stat));
  EXPECT_FALSE(parseDayLineStat("{\"date\":\"2026-13-01\"}", stat));
}

TEST(ReadingAggregateFormat, DayDateHelpersRoundTrip) {
  const int32_t day = static_cast<int32_t>(ChatLog::ChatHistoryFormat::daysFromCivil(2026, 11, 22));
  char date[16];
  ASSERT_TRUE(formatDayDate(date, sizeof(date), day));
  EXPECT_STREQ(date, "2026-11-22");

  char file[24];
  ASSERT_TRUE(formatDayFileName(file, sizeof(file), day));
  EXPECT_STREQ(file, "2026-11-22.jsonl");

  int year = 0;
  int month = 0;
  int dom = 0;
  ASSERT_TRUE(ChatLog::ChatHistoryFormat::parseDayDate(file, year, month, dom));
  EXPECT_EQ(year, 2026);
  EXPECT_EQ(month, 11);
  EXPECT_EQ(dom, 22);
}

TEST(ReadingAggregateFormat, WeekdayFromDayIndex) {
  EXPECT_EQ(isoWeekdayFromDayIndex(0), 4);  // 1970-01-01 was a Thursday
  const int32_t monday = static_cast<int32_t>(ChatLog::ChatHistoryFormat::daysFromCivil(2026, 11, 23));
  EXPECT_EQ(isoWeekdayFromDayIndex(monday), ChatLog::ChatHistoryFormat::isoWeekdayMon1(2026, 11, 23));
  EXPECT_EQ(weekStartMonday(monday), monday);
  EXPECT_EQ(weekStartMonday(monday + 6), monday);  // Sunday belongs to the same column
}

TEST(ReadingAggregateFormat, HeatmapWindowIsMondayAnchored) {
  const int32_t today = static_cast<int32_t>(ChatLog::ChatHistoryFormat::daysFromCivil(2026, 11, 23));
  const int32_t first = heatmapFirstDayIndex(today);
  EXPECT_EQ(isoWeekdayFromDayIndex(first), 1);
  EXPECT_EQ(first + HEATMAP_DAYS - 1, weekStartMonday(today) + 6);
  // Window covers today.
  EXPECT_LE(first, today);
  EXPECT_GE(first + HEATMAP_DAYS - 1, today);
  // 1970-01-01 (Thursday): the first column starts the Monday before.
  EXPECT_EQ(heatmapFirstDayIndex(0), -80);
}

namespace {
struct Sink {
  std::string out;
  bool fail = false;
};

bool sinkWrite(void* ctx, const char* data, const size_t len) {
  auto* sink = static_cast<Sink*>(ctx);
  if (sink->fail) return false;
  sink->out.append(data, len);
  return true;
}

struct LineFeed {
  const char* const* lines = nullptr;
  size_t count = 0;
  size_t index = 0;
};

const char* feedNext(void* ctx) {
  auto* feed = static_cast<LineFeed*>(ctx);
  return feed->index < feed->count ? feed->lines[feed->index++] : nullptr;
}

DayAggregate makeDay(const int year, const int month, const int dom, const uint32_t sec) {
  DayAggregate day;
  day.dayIndex = static_cast<int32_t>(ChatLog::ChatHistoryFormat::daysFromCivil(year, month, dom));
  addSession(day, sec, 1, "/Books/a.epub");
  return day;
}

std::string dayLineOf(const DayAggregate& day) {
  char line[DAY_LINE_CAP];
  const size_t len = formatDayLine(line, sizeof(line), day);
  EXPECT_GT(len, 0u);
  return std::string(line);
}
}  // namespace

TEST(ReadingAggregateMerge, StripsSeparatorCommaFromRecordLength) {
  EXPECT_EQ(dayLineRecordLength("{\"date\":\"x\"},"), 12u);
  EXPECT_EQ(dayLineRecordLength("{\"date\":\"x\"}, \r"), 12u);
  EXPECT_EQ(dayLineRecordLength("{\"date\":\"x\"}"), 12u);
  EXPECT_EQ(dayLineRecordLength(""), 0u);
  EXPECT_EQ(dayLineRecordLength(nullptr), 0u);
}

TEST(ReadingAggregateMerge, EmptySourceEmitsTargetOnly) {
  const DayAggregate day = makeDay(2026, 11, 23, 60);
  const std::string target = dayLineOf(day);
  Sink sink;
  LineFeed feed;
  ASSERT_TRUE(emitMergedDays(day, target.c_str(), target.size(), feedNext, &feed, sinkWrite, &sink));
  EXPECT_EQ(sink.out, target);
}

TEST(ReadingAggregateMerge, ExistingTrailingCommasDoNotAccumulate) {
  const DayAggregate day = makeDay(2026, 11, 21, 30);
  const std::string target = dayLineOf(day);
  // Lines as the previous write left them: every record but the last ends
  // with the JSON separator comma.
  const std::string before = dayLineOf(makeDay(2026, 11, 20, 10)) + ",";
  const std::string after = dayLineOf(makeDay(2026, 11, 22, 20)) + ",";
  const char* lines[] = {before.c_str(), after.c_str()};
  LineFeed feed{lines, 2, 0};
  Sink sink;
  ASSERT_TRUE(emitMergedDays(day, target.c_str(), target.size(), feedNext, &feed, sinkWrite, &sink));

  EXPECT_EQ(sink.out.find(",,"), std::string::npos);
  EXPECT_NE(sink.out.find("2026-11-20"), std::string::npos);
  EXPECT_NE(sink.out.find("2026-11-21"), std::string::npos);
  EXPECT_NE(sink.out.find("2026-11-22"), std::string::npos);
  // Date order survives, with exactly one separator between records.
  EXPECT_LT(sink.out.find("2026-11-20"), sink.out.find("2026-11-21"));
  EXPECT_LT(sink.out.find("2026-11-21"), sink.out.find("2026-11-22"));
  EXPECT_EQ(sink.out.find("2026-11-21", sink.out.find("2026-11-21") + 1), std::string::npos);
}

TEST(ReadingAggregateMerge, ReplacesSameDateOnce) {
  const DayAggregate stale = makeDay(2026, 11, 22, 10);
  const DayAggregate fresh = makeDay(2026, 11, 22, 600);
  const std::string staleLine = dayLineOf(stale) + ",";
  const std::string target = dayLineOf(fresh);
  const char* lines[] = {staleLine.c_str()};
  LineFeed feed{lines, 1, 0};
  Sink sink;
  ASSERT_TRUE(emitMergedDays(fresh, target.c_str(), target.size(), feedNext, &feed, sinkWrite, &sink));

  EXPECT_EQ(sink.out, target);
  EXPECT_NE(sink.out.find("\"total_sec\":600"), std::string::npos);
}

TEST(ReadingAggregateMerge, InsertsBeforeFirstOlderThanTarget) {
  const DayAggregate newest = makeDay(2026, 11, 22, 10);
  const DayAggregate targetDay = makeDay(2026, 11, 19, 5);
  const std::string newestLine = dayLineOf(newest) + ",";
  const std::string target = dayLineOf(targetDay);
  const char* lines[] = {newestLine.c_str()};
  LineFeed feed{lines, 1, 0};
  Sink sink;
  ASSERT_TRUE(emitMergedDays(targetDay, target.c_str(), target.size(), feedNext, &feed, sinkWrite, &sink));
  EXPECT_EQ(sink.out, target + ",\n" + newestLine.substr(0, newestLine.size() - 1));
}

TEST(ReadingAggregateMerge, SkipsNonDayAndUnparsableLines) {
  const DayAggregate day = makeDay(2026, 11, 21, 30);
  const std::string target = dayLineOf(day);
  const char* lines[] = {"{\"schema_version\":1,\"year\":2026,\"days\":[", "{\"date\":\"bad\"}", "garbage"};
  LineFeed feed{lines, 3, 0};
  Sink sink;
  ASSERT_TRUE(emitMergedDays(day, target.c_str(), target.size(), feedNext, &feed, sinkWrite, &sink));
  EXPECT_EQ(sink.out, target);
}

TEST(ReadingAggregateMerge, SinkFailureAborts) {
  const DayAggregate day = makeDay(2026, 11, 21, 30);
  const std::string target = dayLineOf(day);
  Sink sink;
  sink.fail = true;
  LineFeed feed;
  EXPECT_FALSE(emitMergedDays(day, target.c_str(), target.size(), feedNext, &feed, sinkWrite, &sink));
}

TEST(ReadingAggregateFormat, YearPath) {
  char path[64];
  ASSERT_TRUE(formatYearPath(path, sizeof(path), 2026));
  EXPECT_STREQ(path, "/.crosspoint/aggregate/2026.json");
}

}  // namespace
