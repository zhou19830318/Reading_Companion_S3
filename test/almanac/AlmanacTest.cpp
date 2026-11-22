#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Almanac.h"

namespace {

using Almanac::LunarDate;

std::string lunarText(uint16_t y, uint8_t m, uint8_t d) {
  LunarDate lunar;
  if (!Almanac::solarToLunar(y, m, d, lunar)) return "<unsupported>";
  std::string text = Almanac::monthName(lunar.month, lunar.leap);
  text += Almanac::dayName(lunar.day);
  return text;
}

std::string dayPillar(uint16_t y, uint8_t m, uint8_t d) {
  char buf[8] = {};
  Almanac::dayGanZhi(y, m, d, buf, sizeof(buf));
  return buf;
}

std::string monthPillar(uint16_t y, uint8_t m, uint8_t d) {
  char buf[8] = {};
  Almanac::monthGanZhi(y, m, d, buf, sizeof(buf));
  return buf;
}

}  // namespace

// Lunar dates cross-checked against public 万年历 tables (2026 dates from
// huangli.com / tianqi.com listings).
TEST(AlmanacLunar, KnownDates2026) {
  EXPECT_EQ(lunarText(2026, 11, 16), "十月初八");
  EXPECT_EQ(lunarText(2026, 10, 8), "八月廿八");
  EXPECT_EQ(lunarText(2026, 2, 17), "正月初一");
  EXPECT_EQ(lunarText(2026, 2, 18), "正月初二");
  EXPECT_EQ(lunarText(2026, 3, 5), "正月十七");
  EXPECT_EQ(lunarText(2026, 3, 20), "二月初二");
  EXPECT_EQ(lunarText(2026, 4, 5), "二月十八");
  EXPECT_EQ(lunarText(2026, 11, 7), "九月廿九");
  EXPECT_EQ(lunarText(2026, 12, 22), "冬月十四");
}

TEST(AlmanacLunar, LeapMonth2025) {
  // 2025 has a leap 6th month, so the year is 384 days long. A date inside the
  // leap month must report leap = true and the same month number as the normal
  // 6th month before it.
  LunarDate before;
  LunarDate leap;
  ASSERT_TRUE(Almanac::solarToLunar(2025, 7, 20, before));
  ASSERT_TRUE(Almanac::solarToLunar(2025, 8, 20, leap));
  EXPECT_EQ(before.month, 6);
  EXPECT_FALSE(before.leap);
  EXPECT_EQ(leap.month, 6);
  EXPECT_TRUE(leap.leap);
}

TEST(AlmanacLunar, OutOfRangeRejected) {
  LunarDate lunar;
  EXPECT_FALSE(Almanac::solarToLunar(1900, 1, 30, lunar));  // one day before the table starts
  EXPECT_FALSE(Almanac::solarToLunar(2101, 1, 1, lunar));
  EXPECT_TRUE(Almanac::solarToLunar(1900, 1, 31, lunar));
  EXPECT_EQ(lunar.month, 1);
  EXPECT_EQ(lunar.day, 1);
}

// Hong Kong Observatory 2026 solar-term table (day of month).
TEST(AlmanacTerms, MatchesHko2026) {
  static const int expected[24] = {5, 20, 4, 18, 5, 20, 5, 20, 5, 21, 5, 21,
                                   7, 23, 7, 23, 7, 23, 8, 23, 7, 22, 7, 22};
  for (uint8_t i = 0; i < Almanac::TERM_COUNT; i++) {
    EXPECT_EQ(Almanac::solarTermDay(2026, i), expected[i]) << "term index " << static_cast<int>(i);
  }
}

// Cross-year spot checks from the published almanac data.
TEST(AlmanacTerms, SpotChecksOtherYears) {
  EXPECT_EQ(Almanac::solarTermOn(2024, 2, 4), 2);    // 立春
  EXPECT_EQ(Almanac::solarTermOn(2024, 12, 21), 23); // 冬至 2024-12-21
  EXPECT_EQ(Almanac::solarTermOn(2025, 12, 21), 23); // 冬至 2025-12-21
  EXPECT_EQ(Almanac::solarTermOn(2026, 2, 4), 2);    // 立春
}

TEST(AlmanacTerms, OnAndNext) {
  EXPECT_EQ(Almanac::solarTermOn(2026, 11, 7), 20);   // 立冬
  EXPECT_EQ(Almanac::solarTermOn(2026, 11, 22), 21);  // 小雪
  EXPECT_EQ(Almanac::solarTermOn(2026, 11, 16), -1);

  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  const int next = Almanac::nextSolarTerm(2026, 11, 16, year, month, day);
  EXPECT_EQ(next, 21);
  EXPECT_EQ(year, 2026);
  EXPECT_EQ(month, 11);
  EXPECT_EQ(day, 22);

  // Across a year boundary: the next term after 冬至 is 小寒 of the next year.
  const int after = Almanac::nextSolarTerm(2026, 12, 25, year, month, day);
  EXPECT_EQ(after, 0);
  EXPECT_EQ(year, 2027);
  EXPECT_EQ(month, 1);
  EXPECT_EQ(day, 5);
}

TEST(AlmanacGanZhi, DayPillar) {
  // Anchors cross-checked against two published almanacs: qxsj66 gives
  // 2026-11-18 = 丙申; laohuangli gives 2026-07-01 = 丙子. The (JDN+49)%60
  // anchor reproduces both, and the listing steps by one pillar per day.
  EXPECT_EQ(dayPillar(2026, 7, 1), "丙子");
  EXPECT_EQ(dayPillar(2026, 11, 16), "甲午");
  EXPECT_EQ(dayPillar(2026, 11, 17), "乙未");
  EXPECT_EQ(dayPillar(2026, 11, 18), "丙申");
  // 60-day cycle: exactly 60 days later is the same pillar.
  EXPECT_EQ(dayPillar(2027, 1, 17), "丙申");
}

TEST(AlmanacGanZhi, MonthAndYearPillar) {
  EXPECT_EQ(monthPillar(2026, 11, 16), "己亥");
  EXPECT_EQ(monthPillar(2026, 3, 4), "庚寅");   // 惊蛰 is on 3-05: 3-04 is still 寅月
  EXPECT_EQ(monthPillar(2026, 3, 5), "辛卯");   // 惊蛰: 卯月 starts
  EXPECT_EQ(std::string(Almanac::yearGanZhi(2026)), "丙午");
  EXPECT_EQ(std::string(Almanac::zodiac(2026)), "马");
  // A January date before 立春 still belongs to the previous 干支 year.
  EXPECT_EQ(std::string(Almanac::yearGanZhi(2025)), "乙巳");
}

TEST(AlmanacJianChu, OfficerFollowsMonthBranch) {
  // 2026-11-16 is 亥月 (立冬) + 午日: 建 at 亥, so 午 is the 8th officer, 危.
  EXPECT_STREQ(Almanac::jianChu(2026, 11, 16), "危");
  // The day before is 巳 -> 破.
  EXPECT_STREQ(Almanac::jianChu(2026, 11, 15), "破");
  // The 立春 boundary must move the officer by exactly one step (the month
  // branch advances, the day branch does not).
  const char* before = Almanac::jianChu(2026, 2, 3);
  const char* after = Almanac::jianChu(2026, 2, 4);
  static const char* const officers[12] = {"建", "除", "满", "平", "定", "执", "破", "危", "成", "收", "开", "闭"};
  int beforeIndex = -1;
  int afterIndex = -1;
  for (int i = 0; i < 12; i++) {
    if (strcmp(officers[i], before) == 0) beforeIndex = i;
    if (strcmp(officers[i], after) == 0) afterIndex = i;
  }
  ASSERT_GE(beforeIndex, 0);
  // On the 节 boundary both the day branch and the month branch advance by one,
  // so the officer carries over unchanged: 立春 moves the pillars a reader sees,
  // not the 建除 value.
  EXPECT_EQ(afterIndex, beforeIndex);
  EXPECT_STRNE(Almanac::yi(2026, 11, 16), Almanac::ji(2026, 11, 16));
}

TEST(AlmanacDayFields, ChongShaNaYinAndDirections) {
  char buf[16] = {};
  // 2026-11-18 is 丙申 (qxsj66): clashes with 寅 (tiger), 申子辰 group -> 煞南;
  // 纳音 山下火; 丙 stem -> 财神西南 / 喜神西南.
  Almanac::dayChongSha(2026, 11, 18, buf, sizeof(buf));
  EXPECT_STREQ(buf, "冲虎煞南");
  EXPECT_STREQ(Almanac::dayNaYin(2026, 11, 18), "山下火");
  EXPECT_STREQ(Almanac::dayWealthDirection(2026, 11, 18), "西南");
  EXPECT_STREQ(Almanac::dayJoyDirection(2026, 11, 18), "西南");
  // 2026-07-01 is 丙子 (laohuangli): clashes with 午 (horse), 申子辰 -> 煞南;
  // 纳音 涧下水; 丙 stem -> 财神西南 / 喜神西南.
  Almanac::dayChongSha(2026, 7, 1, buf, sizeof(buf));
  EXPECT_STREQ(buf, "冲马煞南");
  EXPECT_STREQ(Almanac::dayNaYin(2026, 7, 1), "涧下水");
  EXPECT_STREQ(Almanac::dayWealthDirection(2026, 7, 1), "西南");
  EXPECT_STREQ(Almanac::dayJoyDirection(2026, 7, 1), "西南");
}

TEST(AlmanacSolarTerms, PrevAndNextBracketTheDate) {
  uint16_t ty = 0;
  uint8_t tm = 0;
  uint8_t td = 0;
  const int prev = Almanac::prevSolarTerm(2026, 11, 18, ty, tm, td);
  EXPECT_GE(prev, 0);
  EXPECT_STREQ(Almanac::solarTermName(static_cast<uint8_t>(prev)), "立冬");
  EXPECT_EQ(ty, 2026);
  EXPECT_EQ(tm, 11);
  EXPECT_EQ(td, 7);
  const int next = Almanac::nextSolarTerm(2026, 11, 18, ty, tm, td);
  EXPECT_GE(next, 0);
  EXPECT_STREQ(Almanac::solarTermName(static_cast<uint8_t>(next)), "小雪");
  EXPECT_EQ(tm, 11);
  EXPECT_EQ(td, 22);
  // On a term day itself, prev is strictly before and next strictly after.
  const int onPrev = Almanac::prevSolarTerm(2026, 11, 7, ty, tm, td);
  EXPECT_NE(onPrev, prev);
}
