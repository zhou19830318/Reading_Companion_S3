#include <ReadingPlanFormat.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace ReadingPlan;

namespace {

std::string serialize(const Plans& plans) {
  char buffer[4096];
  const size_t len = formatPlansFile(buffer, sizeof(buffer), plans);
  return len == 0 ? std::string() : std::string(buffer, len);
}

std::string serialize(const Slots& slots, const char* updatedAt = "2026-11-23T09:00:00+08:00") {
  char buffer[2048];
  const size_t len = formatSlotsFile(buffer, sizeof(buffer), slots, updatedAt);
  return len == 0 ? std::string() : std::string(buffer, len);
}

int parsePlans(const std::string& text, Plans& out) { return parsePlansFile(text.data(), text.size(), out); }

int parseSlots(const std::string& text, Slots& out) { return parseSlotsFile(text.data(), text.size(), out); }

}  // namespace

TEST(ReadingPlanFormat, ValidatesCalendarDates) {
  EXPECT_TRUE(isValidDate("2026-11-23"));
  EXPECT_TRUE(isValidDate("2028-02-29"));
  EXPECT_FALSE(isValidDate("2027-02-29"));
  EXPECT_FALSE(isValidDate("2026-13-01"));
  EXPECT_FALSE(isValidDate("2026-00-10"));
  EXPECT_FALSE(isValidDate("2026-04-31"));
  EXPECT_FALSE(isValidDate("2026-4-1"));
  EXPECT_FALSE(isValidDate("2026-11-23 "));
  EXPECT_FALSE(isValidDate(""));
  EXPECT_FALSE(isValidDate(nullptr));
}

TEST(ReadingPlanFormat, ValidatesClockTimes) {
  EXPECT_TRUE(isValidTime("00:00"));
  EXPECT_TRUE(isValidTime("23:59"));
  EXPECT_FALSE(isValidTime("24:00"));
  EXPECT_FALSE(isValidTime("12:60"));
  EXPECT_FALSE(isValidTime("9:30"));
  EXPECT_FALSE(isValidTime("09:30:00"));
  EXPECT_FALSE(isValidTime(nullptr));

  uint16_t minutes = 0;
  EXPECT_TRUE(parseHhMm("21:15", minutes));
  EXPECT_EQ(minutes, static_cast<uint16_t>(21 * 60 + 15));
  char formatted[8];
  formatHhMm(formatted, sizeof(formatted), minutes);
  EXPECT_STREQ(formatted, "21:15");
}

TEST(ReadingPlanFormat, ParsesKnownStatusValues) {
  PlanStatus status = PlanStatus::Archived;
  EXPECT_TRUE(parseStatus("active", 6, status));
  EXPECT_EQ(status, PlanStatus::Active);
  EXPECT_TRUE(parseStatus("paused", 6, status));
  EXPECT_EQ(status, PlanStatus::Paused);
  EXPECT_TRUE(parseStatus("archived", 8, status));
  EXPECT_EQ(status, PlanStatus::Archived);
  EXPECT_FALSE(parseStatus("done", 4, status));
  EXPECT_FALSE(parseStatus("", 0, status));
  EXPECT_STREQ(statusKey(PlanStatus::Archived), "archived");
}

TEST(ReadingPlanFormat, PlansRoundTrip) {
  Plans plans;
  plans.count = 2;

  Plan& first = plans.items[0];
  snprintf(first.id, sizeof(first.id), "plan_20261123_001");
  snprintf(first.bookId, sizeof(first.bookId), "/books/\xE4\xB8\xAD\xE6\x96\x87 epub.epub");
  snprintf(first.createdAt, sizeof(first.createdAt), "2026-11-23T09:10:11+08:00");
  snprintf(first.targetDate, sizeof(first.targetDate), "2026-12-31");
  first.weeklyMinutes = 180;
  first.weeklyPages = 250;
  first.priority = 2;
  first.tzOffsetQ = 80;
  first.status = PlanStatus::Paused;

  Plan& second = plans.items[1];
  snprintf(second.id, sizeof(second.id), "plan_20261123_002");
  second.weeklyMinutes = 45;
  second.tzOffsetQ = -20;
  second.status = PlanStatus::Archived;
  snprintf(second.pausedUntil, sizeof(second.pausedUntil), "2026-12-01");

  Plans parsed;
  ASSERT_EQ(parsePlans(serialize(plans), parsed), 2);
  ASSERT_EQ(parsed.count, 2);

  EXPECT_STREQ(parsed.items[0].id, first.id);
  EXPECT_STREQ(parsed.items[0].bookId, first.bookId);
  EXPECT_STREQ(parsed.items[0].createdAt, first.createdAt);
  EXPECT_STREQ(parsed.items[0].targetDate, first.targetDate);
  EXPECT_EQ(parsed.items[0].weeklyMinutes, 180);
  EXPECT_EQ(parsed.items[0].weeklyPages, 250);
  EXPECT_EQ(parsed.items[0].priority, 2);
  EXPECT_EQ(parsed.items[0].tzOffsetQ, 80);
  EXPECT_EQ(parsed.items[0].status, PlanStatus::Paused);

  EXPECT_STREQ(parsed.items[1].id, second.id);
  EXPECT_EQ(parsed.items[1].weeklyMinutes, 45);
  EXPECT_EQ(parsed.items[1].tzOffsetQ, -20);
  EXPECT_EQ(parsed.items[1].status, PlanStatus::Archived);
  EXPECT_STREQ(parsed.items[1].pausedUntil, "2026-12-01");
  EXPECT_EQ(parsed.items[1].bookId[0], '\0');
}

TEST(ReadingPlanFormat, EscapesBookPathsAndSlotNames) {
  Plans plans;
  plans.count = 1;
  snprintf(plans.items[0].id, sizeof(plans.items[0].id), "plan_1");
  snprintf(plans.items[0].bookId, sizeof(plans.items[0].bookId), "/a\"b\\c\nd.epub");

  Plans parsed;
  ASSERT_EQ(parsePlans(serialize(plans), parsed), 1);
  EXPECT_STREQ(parsed.items[0].bookId, "/a\"b\\c\nd.epub");

  Slots slots;
  slots.count = 1;
  snprintf(slots.items[0].name, sizeof(slots.items[0].name), "\xE5\xB7\xA5\xE4\xBD\x9C\xE6\x97\xA5 \"x\"");
  slots.items[0].daysMask = static_cast<uint8_t>(1u << 1);
  slots.items[0].startMin = 21 * 60;
  slots.items[0].endMin = 22 * 60 + 30;

  Slots parsedSlots;
  ASSERT_EQ(parseSlots(serialize(slots), parsedSlots), 1);
  EXPECT_STREQ(parsedSlots.items[0].name, slots.items[0].name);
}

TEST(ReadingPlanFormat, EmptyPlansFileIsValid) {
  Plans empty;
  const std::string text = serialize(empty);
  ASSERT_FALSE(text.empty());
  EXPECT_NE(text.find("\"plans\":[]"), std::string::npos);

  Plans parsed;
  parsed.count = 7;
  EXPECT_EQ(parsePlans(text, parsed), 0);
  EXPECT_EQ(parsed.count, 0);
}

TEST(ReadingPlanFormat, SkipsGarbageAndInvalidRecords) {
  const std::string text =
      "{\"schema_version\":1,\"plans\":[\n"
      "not json\n"
      "{\"book_id\":\"/no-id.epub\"}\n"
      "{\"id\":\"ok\",\"book_id\":\"/b.epub\",\"status\":\"paused\",\"target_date\":\"2026-12-31\"}\n"
      "{\"id\":\"bad-date\",\"target_date\":\"2026-02-30\"}\n"
      "]}";

  Plans parsed;
  EXPECT_EQ(parsePlans(text, parsed), 1);
  ASSERT_EQ(parsed.count, 1);
  EXPECT_STREQ(parsed.items[0].id, "ok");
  EXPECT_STREQ(parsed.items[0].bookId, "/b.epub");
  EXPECT_EQ(parsed.items[0].status, PlanStatus::Paused);
  EXPECT_EQ(parsed.items[0].targetDate[0] != '\0', true);
}

TEST(ReadingPlanFormat, ClampsOutOfRangeFields) {
  const std::string text =
      "{\"schema_version\":1,\"plans\":[\n"
      "{\"id\":\"p\",\"goal\":{\"type\":\"minutes_per_week\",\"value\":99999,\"pages_per_week\":99999},"
      "\"priority\":9,\"timezone_offset_q\":-200}\n"
      "]}";

  Plans parsed;
  ASSERT_EQ(parsePlans(text, parsed), 1);
  EXPECT_EQ(parsed.items[0].weeklyMinutes, 10080);
  EXPECT_EQ(parsed.items[0].weeklyPages, 10000);
  EXPECT_EQ(parsed.items[0].priority, 5);
  EXPECT_EQ(parsed.items[0].tzOffsetQ, -48);
}

TEST(ReadingPlanFormat, UnknownStatusFallsBackToActive) {
  const std::string text =
      "{\"schema_version\":1,\"plans\":[\n"
      "{\"id\":\"p\",\"status\":\"someday\"}\n"
      "]}";
  Plans parsed;
  ASSERT_EQ(parsePlans(text, parsed), 1);
  EXPECT_EQ(parsed.items[0].status, PlanStatus::Active);
}

TEST(ReadingPlanFormat, CapsRecordCountAtMaximum) {
  // A hand-written file may carry more records than the cap; the reader keeps
  // the first MAX_PLANS and ignores the rest instead of overflowing.
  std::string text = "{\"schema_version\":1,\"plans\":[\n";
  for (int i = 0; i < MAX_PLANS + 3; ++i) {
    text += "{\"id\":\"p" + std::to_string(i) + "\"}\n";
  }
  text += "]}";

  Plans parsed;
  EXPECT_EQ(parsePlans(text, parsed), MAX_PLANS);
  EXPECT_EQ(parsed.count, MAX_PLANS);
  EXPECT_STREQ(parsed.items[MAX_PLANS - 1].id, "p7");
}

TEST(ReadingPlanFormat, FailsOnTooSmallBuffer) {
  Plans plans;
  plans.count = 1;
  snprintf(plans.items[0].id, sizeof(plans.items[0].id), "plan_20261123_001");
  snprintf(plans.items[0].bookId, sizeof(plans.items[0].bookId), "/books/a-very-long-book-name.epub");
  plans.items[0].weeklyMinutes = 180;

  char small[40];
  EXPECT_EQ(formatPlansFile(small, sizeof(small), plans), 0);
}

TEST(ReadingPlanFormat, GeneratesStablePlanIds) {
  char id[ID_CAP];
  makePlanId(id, sizeof(id), "2026-12-31", 3);
  EXPECT_STREQ(id, "plan_20261231_003");

  makePlanId(id, sizeof(id), "", 1);
  EXPECT_STREQ(id, "plan_00000000_001");

  makePlanId(id, sizeof(id), "not-a-date", 1200);
  EXPECT_STREQ(id, "plan_00000000_999");
}

TEST(ReadingPlanFormat, SlotsRoundTrip) {
  Slots slots;
  slots.count = 2;

  FreeSlot& weekdays = slots.items[0];
  snprintf(weekdays.name, sizeof(weekdays.name), "\xE5\xB7\xA5\xE4\xBD\x9C\xE6\x97\xA5\xE6\x99\x9A\xE4\xB8\x8A");
  weekdays.daysMask = static_cast<uint8_t>(dayBit(1) | dayBit(2) | dayBit(3) | dayBit(4) | dayBit(5));
  weekdays.startMin = 21 * 60;
  weekdays.endMin = 22 * 60 + 30;
  weekdays.flexible = false;

  FreeSlot& weekend = slots.items[1];
  snprintf(weekend.name, sizeof(weekend.name), "weekend");
  weekend.daysMask = static_cast<uint8_t>(dayBit(0) | dayBit(6));
  weekend.startMin = 10 * 60;
  weekend.endMin = 11 * 60 + 30;
  weekend.flexible = true;

  Slots parsed;
  ASSERT_EQ(parseSlots(serialize(slots), parsed), 2);
  ASSERT_EQ(parsed.count, 2);

  EXPECT_STREQ(parsed.items[0].name, weekdays.name);
  EXPECT_EQ(parsed.items[0].daysMask, weekdays.daysMask);
  EXPECT_EQ(parsed.items[0].startMin, static_cast<uint16_t>(21 * 60));
  EXPECT_EQ(parsed.items[0].endMin, static_cast<uint16_t>(22 * 60 + 30));
  EXPECT_FALSE(parsed.items[0].flexible);

  EXPECT_STREQ(parsed.items[1].name, "weekend");
  EXPECT_EQ(parsed.items[1].daysMask, static_cast<uint8_t>(0x41));
  EXPECT_TRUE(parsed.items[1].flexible);
}

TEST(ReadingPlanFormat, SlotsRejectInvalidWindows) {
  const std::string noDays =
      "{\"schema_version\":1,\"slots\":[\n"
      "{\"name\":\"x\",\"days\":[],\"start\":\"09:00\",\"end\":\"10:00\"}\n"
      "]}";
  Slots parsed;
  EXPECT_EQ(parseSlots(noDays, parsed), 0);

  const std::string reversed =
      "{\"schema_version\":1,\"slots\":[\n"
      "{\"name\":\"x\",\"days\":[1],\"start\":\"10:00\",\"end\":\"09:00\"}\n"
      "]}";
  EXPECT_EQ(parseSlots(reversed, parsed), 0);

  const std::string badTime =
      "{\"schema_version\":1,\"slots\":[\n"
      "{\"name\":\"x\",\"days\":[1],\"start\":\"25:00\",\"end\":\"26:00\"}\n"
      "]}";
  EXPECT_EQ(parseSlots(badTime, parsed), 0);

  const std::string badDay =
      "{\"schema_version\":1,\"slots\":[\n"
      "{\"name\":\"x\",\"days\":[7],\"start\":\"09:00\",\"end\":\"10:00\"}\n"
      "]}";
  EXPECT_EQ(parseSlots(badDay, parsed), 0);
}

TEST(ReadingPlanFormat, EmptySlotsFileIsValid) {
  Slots empty;
  Slots parsed;
  parsed.count = 4;
  EXPECT_EQ(parseSlots(serialize(empty), parsed), 0);
  EXPECT_EQ(parsed.count, 0);
}

TEST(ReadingPlanFormat, DayMaskHelpers) {
  EXPECT_EQ(dayBit(-1), 0);
  EXPECT_EQ(dayBit(7), 0);
  EXPECT_EQ(dayBit(0), 1);
  EXPECT_EQ(dayBit(6), 64);
  EXPECT_TRUE(daySelected(0x41, 0));
  EXPECT_FALSE(daySelected(0x41, 1));
  EXPECT_EQ(dayCount(0x41), 2);
  EXPECT_EQ(dayCount(0), 0);

  // Writer and reader agree on the Sunday-first encoding.
  Slots slots;
  slots.count = 1;
  snprintf(slots.items[0].name, sizeof(slots.items[0].name), "s");
  slots.items[0].daysMask = 0x41;
  slots.items[0].startMin = 0;
  slots.items[0].endMin = 1;
  const std::string text = serialize(slots);
  EXPECT_NE(text.find("\"days\":[0,6]"), std::string::npos);
}
