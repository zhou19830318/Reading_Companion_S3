#include <ReadingReminder.h>
#include <gtest/gtest.h>

using namespace ReadingPlan;

namespace {
constexpr bool ON = true;
constexpr bool OFF = false;
constexpr bool KNOWN = true;
constexpr bool PLAN = true;
constexpr bool SKIPPED = false;
}  // namespace

TEST(ReadingReminder, HiddenUnlessEverythingLinesUp) {
  EXPECT_EQ(buildReminder(OFF, KNOWN, PLAN, SKIPPED, 0, 30).kind, ReminderStatus::Kind::Hidden);
  EXPECT_EQ(buildReminder(ON, !KNOWN, PLAN, SKIPPED, 0, 30).kind, ReminderStatus::Kind::Hidden);
  EXPECT_EQ(buildReminder(ON, KNOWN, !PLAN, SKIPPED, 0, 30).kind, ReminderStatus::Kind::Hidden);
  EXPECT_EQ(buildReminder(ON, KNOWN, PLAN, !SKIPPED, 0, 30).kind, ReminderStatus::Kind::Hidden);
  EXPECT_EQ(buildReminder(ON, KNOWN, PLAN, SKIPPED, 0, 0).kind, ReminderStatus::Kind::Hidden);
}

TEST(ReadingReminder, DoneAtTheDailyTarget) {
  EXPECT_EQ(buildReminder(ON, KNOWN, PLAN, SKIPPED, 30 * 60, 30).kind, ReminderStatus::Kind::Done);
  EXPECT_EQ(buildReminder(ON, KNOWN, PLAN, SKIPPED, 45 * 60, 30).kind, ReminderStatus::Kind::Done);
}

TEST(ReadingReminder, RoundsMissingMinutesUp) {
  const auto status = buildReminder(ON, KNOWN, PLAN, SKIPPED, 25 * 60, 30);
  ASSERT_EQ(status.kind, ReminderStatus::Kind::Remaining);
  EXPECT_EQ(status.remainingMinutes, 5);
  EXPECT_EQ(status.totalMinutes, 30);

  // 1 second short of 25:00 leaves 5:01, which rounds up to 6; a second past
  // 25:00 leaves 4:59, which rounds up to 5.
  const auto nearly = buildReminder(ON, KNOWN, PLAN, SKIPPED, 25 * 60 - 1, 30);
  EXPECT_EQ(nearly.remainingMinutes, 6);
  const auto justOver = buildReminder(ON, KNOWN, PLAN, SKIPPED, 25 * 60 + 1, 30);
  EXPECT_EQ(justOver.remainingMinutes, 5);
}

TEST(ReadingReminder, WholePlanLeftWhenNothingRead) {
  const auto status = buildReminder(ON, KNOWN, PLAN, SKIPPED, 0, 26);
  ASSERT_EQ(status.kind, ReminderStatus::Kind::Remaining);
  EXPECT_EQ(status.remainingMinutes, 26);
}

TEST(ReadingReminder, NeverReportsZeroWhenNotDone) {
  // 30 min target, 29 min 59 s read: one minute remains, not zero.
  const auto status = buildReminder(ON, KNOWN, PLAN, SKIPPED, 30 * 60 - 1, 30);
  EXPECT_EQ(status.kind, ReminderStatus::Kind::Remaining);
  EXPECT_EQ(status.remainingMinutes, 1);
}
