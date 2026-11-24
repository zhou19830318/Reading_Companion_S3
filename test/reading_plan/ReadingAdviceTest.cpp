#include <ReadingAdvice.h>
#include <gtest/gtest.h>

using namespace ReadingPlan;

namespace {

Plan makePlan(uint16_t weeklyMinutes, uint16_t weeklyPages = 0) {
  Plan plan;
  plan.weeklyMinutes = weeklyMinutes;
  plan.weeklyPages = weeklyPages;
  return plan;
}

FreeSlot makeSlot(uint8_t daysMask, uint16_t startMin, uint16_t endMin) {
  FreeSlot slot;
  slot.daysMask = daysMask;
  slot.startMin = startMin;
  slot.endMin = endMin;
  return slot;
}

Slots oneSlot(const FreeSlot& slot) {
  Slots slots;
  slots.items[0] = slot;
  slots.count = 1;
  return slots;
}

PaceWindow pace(uint32_t totalSec, uint32_t pages) {
  PaceWindow window;
  window.totalSec = totalSec;
  window.pagesAdvanced = pages;
  window.daysWithData = 1;
  return window;
}

constexpr int MONDAY = 1;  // daysMask order: 0 = Sunday

}  // namespace

TEST(ReadingAdvice, FallsBackToWeeklyMinutesWithoutHistory) {
  const auto advice = buildAdvice(makePlan(180), Slots{}, PaceWindow{}, MONDAY, 0, 0);
  EXPECT_EQ(advice.dailyMinutes, 26);  // ceil(180 / 7)
  EXPECT_FALSE(advice.hasPace);
  EXPECT_EQ(advice.pagesPerDay, 0);
  EXPECT_EQ(advice.segmentCount, 0);
}

TEST(ReadingAdvice, RoundsWeeklyMinutesUp) {
  // 7 x 25 = 175 would be below the 180 min goal; rounding up keeps the plan.
  EXPECT_EQ(buildAdvice(makePlan(175), Slots{}, PaceWindow{}, MONDAY, 0, 0).dailyMinutes, 25);
  EXPECT_EQ(buildAdvice(makePlan(176), Slots{}, PaceWindow{}, MONDAY, 0, 0).dailyMinutes, 26);
}

TEST(ReadingAdvice, TranslatesPagesGoalThroughMeasuredPace) {
  // 140 pages/week -> 20 pages/day; 40 pages in 20 min = 2.00 pages/min.
  const auto advice = buildAdvice(makePlan(30, 140), Slots{}, pace(1200, 40), MONDAY, 0, 0);
  EXPECT_TRUE(advice.hasPace);
  EXPECT_EQ(advice.pacePagesPerMinX100, 200);
  EXPECT_EQ(advice.pagesPerDay, 20);
  EXPECT_EQ(advice.dailyMinutes, 10);
}

TEST(ReadingAdvice, PagesGoalWithoutPaceKeepsWeeklyMinutes) {
  const auto advice = buildAdvice(makePlan(210, 140), Slots{}, PaceWindow{}, MONDAY, 0, 0);
  EXPECT_FALSE(advice.hasPace);
  EXPECT_EQ(advice.pagesPerDay, 20);
  EXPECT_EQ(advice.dailyMinutes, 30);  // ceil(210 / 7)
}

TEST(ReadingAdvice, ClampsExtremeAdvice) {
  EXPECT_EQ(buildAdvice(makePlan(2000), Slots{}, PaceWindow{}, MONDAY, 0, 0).dailyMinutes, 240);
  EXPECT_EQ(buildAdvice(makePlan(0), Slots{}, PaceWindow{}, MONDAY, 0, 0).dailyMinutes, 5);
  // A very fast pace must not schedule a negative/zero day.
  const auto fast = buildAdvice(makePlan(30, 70), Slots{}, pace(600, 2000), MONDAY, 0, 0);
  EXPECT_GE(fast.dailyMinutes, 5);
}

TEST(ReadingAdvice, IgnoresUnreliablePaceSamples) {
  // 5 seconds of data or zero pages is not a pace.
  EXPECT_FALSE(buildAdvice(makePlan(30), Slots{}, pace(5, 10), MONDAY, 0, 0).hasPace);
  EXPECT_FALSE(buildAdvice(makePlan(30), Slots{}, pace(600, 0), MONDAY, 0, 0).hasPace);
  EXPECT_FALSE(buildAdvice(makePlan(30), Slots{}, pace(0, 0), MONDAY, 0, 0).hasPace);
}

TEST(ReadingAdvice, SplitsDailyMinutesIntoOneTodaysSlot) {
  const Slots slots = oneSlot(makeSlot(dayBit(MONDAY), 21 * 60, 22 * 60));
  const auto advice = buildAdvice(makePlan(180), slots, PaceWindow{}, MONDAY, 0, 0);
  ASSERT_EQ(advice.segmentCount, 1);
  EXPECT_EQ(advice.segments[0].startMin, 21 * 60);
  EXPECT_EQ(advice.segments[0].endMin, 21 * 60 + 26);
  EXPECT_EQ(advice.scheduledMinutes, 26);
}

TEST(ReadingAdvice, ChunksLongAdviceAtThirtyMinutes) {
  const Slots slots = oneSlot(makeSlot(dayBit(MONDAY), 20 * 60, 23 * 60));
  const auto advice = buildAdvice(makePlan(525), slots, PaceWindow{}, MONDAY, 0, 0);
  ASSERT_EQ(advice.dailyMinutes, 75);
  ASSERT_EQ(advice.segmentCount, 3);
  EXPECT_EQ(advice.segments[0].startMin, 20 * 60);
  EXPECT_EQ(advice.segments[0].endMin, 20 * 60 + 30);
  EXPECT_EQ(advice.segments[1].startMin, 20 * 60 + 30);
  EXPECT_EQ(advice.segments[2].endMin, 20 * 60 + 75);
  EXPECT_EQ(advice.scheduledMinutes, 75);
}

TEST(ReadingAdvice, DropsATailShorterThanTenMinutes) {
  // 65 min -> 30 + 30, the last 5 min block is not worth a slot.
  const Slots slots = oneSlot(makeSlot(dayBit(MONDAY), 20 * 60, 23 * 60));
  const auto advice = buildAdvice(makePlan(455), slots, PaceWindow{}, MONDAY, 0, 0);
  ASSERT_EQ(advice.dailyMinutes, 65);
  EXPECT_EQ(advice.segmentCount, 2);
  EXPECT_EQ(advice.scheduledMinutes, 60);
}

TEST(ReadingAdvice, CapsSegmentsAtThree) {
  const Slots slots = oneSlot(makeSlot(dayBit(MONDAY), 8 * 60, 18 * 60));
  const auto advice = buildAdvice(makePlan(1260), slots, PaceWindow{}, MONDAY, 0, 0);
  EXPECT_EQ(advice.dailyMinutes, 180);
  EXPECT_EQ(advice.segmentCount, 3);
  EXPECT_EQ(advice.scheduledMinutes, 90);
}

TEST(ReadingAdvice, UsesEveryTodaysSlotInOrder) {
  Slots slots;
  slots.items[0] = makeSlot(dayBit(MONDAY), 18 * 60, 18 * 60 + 20);
  slots.items[1] = makeSlot(dayBit(MONDAY) | dayBit(2), 21 * 60, 22 * 60);
  slots.count = 2;
  const auto advice = buildAdvice(makePlan(210), slots, PaceWindow{}, MONDAY, 0, 0);
  ASSERT_EQ(advice.segmentCount, 2);
  EXPECT_EQ(advice.segments[0].startMin, 18 * 60);
  EXPECT_EQ(advice.segments[0].endMin, 18 * 60 + 20);
  EXPECT_EQ(advice.segments[1].startMin, 21 * 60);
  EXPECT_EQ(advice.segments[1].endMin, 21 * 60 + 10);
}

TEST(ReadingAdvice, SkipsSlotsOnOtherDays) {
  Slots slots;
  slots.items[0] = makeSlot(dayBit(2), 21 * 60, 22 * 60);  // Tuesday
  slots.count = 1;
  const auto advice = buildAdvice(makePlan(180), slots, PaceWindow{}, MONDAY, 0, 0);
  EXPECT_EQ(advice.segmentCount, 0);
}

TEST(ReadingAdvice, NoSegmentsWhenWeekdayUnknown) {
  const Slots slots = oneSlot(makeSlot(0x7F, 21 * 60, 22 * 60));
  const auto advice = buildAdvice(makePlan(180), slots, PaceWindow{}, -1, 0, 0);
  EXPECT_EQ(advice.dailyMinutes, 26);
  EXPECT_EQ(advice.segmentCount, 0);
}

TEST(ReadingAdvice, ReportsDaysLeftOnlyForFutureTargets) {
  EXPECT_EQ(buildAdvice(makePlan(180), Slots{}, PaceWindow{}, MONDAY, 20800, 20795).daysLeft, 5);
  EXPECT_EQ(buildAdvice(makePlan(180), Slots{}, PaceWindow{}, MONDAY, 20795, 20795).daysLeft, 0);
  EXPECT_EQ(buildAdvice(makePlan(180), Slots{}, PaceWindow{}, MONDAY, 20700, 20795).daysLeft, 0);
  EXPECT_EQ(buildAdvice(makePlan(180), Slots{}, PaceWindow{}, MONDAY, 0, 0).daysLeft, 0);
}

TEST(ReadingAdvice, IgnoresEmptyAndInvertedSlots) {
  Slots slots;
  slots.items[0] = makeSlot(dayBit(MONDAY), 22 * 60, 22 * 60);
  slots.items[1] = makeSlot(dayBit(MONDAY), 23 * 60, 22 * 60);
  slots.count = 2;
  EXPECT_EQ(buildAdvice(makePlan(180), slots, PaceWindow{}, MONDAY, 0, 0).segmentCount, 0);
}
