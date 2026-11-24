#include <ReadingSessionTracker.h>
#include <gtest/gtest.h>

using ReadingHistory::IDLE_TIMEOUT_SEC;
using ReadingHistory::MIN_SANE_EPOCH_SEC;
using ReadingHistory::SessionRecord;
using ReadingHistory::SessionTracker;

namespace {

constexpr int64_t T0 = 1'700'000'000;  // sane, comfortably after MIN_SANE_EPOCH_SEC

}  // namespace

TEST(ReadingSessionTracker, StartRejectsUnsyncedClock) {
  SessionTracker tracker;
  EXPECT_FALSE(tracker.start(MIN_SANE_EPOCH_SEC - 1));
  EXPECT_FALSE(tracker.active());
  SessionRecord record;
  EXPECT_FALSE(tracker.finish(T0, record));
}

TEST(ReadingSessionTracker, StartThenFinishRecordsDuration) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));
  ASSERT_TRUE(tracker.active());

  SessionRecord record;
  ASSERT_TRUE(tracker.finish(T0 + 125, record));
  EXPECT_EQ(record.startSec, T0);
  EXPECT_EQ(record.endSec, T0 + 125);
  EXPECT_EQ(record.pagesAdvanced, 0u);
  EXPECT_TRUE(record.valid());
  EXPECT_FALSE(tracker.active());
}

TEST(ReadingSessionTracker, ZeroLengthSessionIsRejected) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));
  SessionRecord record;
  EXPECT_FALSE(tracker.finish(T0, record));
  EXPECT_FALSE(tracker.active());
}

TEST(ReadingSessionTracker, IdleTickClosesAtIdleBoundary) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));

  SessionRecord record;
  EXPECT_FALSE(tracker.tick(T0 + IDLE_TIMEOUT_SEC - 1, record));
  EXPECT_TRUE(tracker.active());

  ASSERT_TRUE(tracker.tick(T0 + IDLE_TIMEOUT_SEC + 600, record));
  EXPECT_EQ(record.startSec, T0);
  EXPECT_EQ(record.endSec, T0 + IDLE_TIMEOUT_SEC);
  EXPECT_FALSE(tracker.active());
}

TEST(ReadingSessionTracker, ActivityPostponesIdleClose) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));

  tracker.noteActivity(T0 + 60);
  SessionRecord record;
  EXPECT_FALSE(tracker.tick(T0 + 140, record));  // 80 s after last activity
  ASSERT_TRUE(tracker.tick(T0 + 150, record));   // 90 s after last activity
  EXPECT_EQ(record.endSec, T0 + 60 + IDLE_TIMEOUT_SEC);
}

TEST(ReadingSessionTracker, InputAfterIdleDoesNotRestartSession) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));

  SessionRecord first;
  ASSERT_TRUE(tracker.tick(T0 + IDLE_TIMEOUT_SEC, first));
  EXPECT_FALSE(tracker.active());

  // Opening a menu or leaving the book must not fabricate a 1-second session.
  tracker.noteActivity(T0 + 600);
  EXPECT_FALSE(tracker.active());

  // The next forward page turn is the real reading signal and starts one.
  tracker.notePageAdvance(T0 + 610);
  EXPECT_TRUE(tracker.active());
  SessionRecord second;
  ASSERT_TRUE(tracker.finish(T0 + 620, second));
  EXPECT_EQ(second.startSec, T0 + 610);
  EXPECT_EQ(second.pagesAdvanced, 1u);
}

TEST(ReadingSessionTracker, PageAdvancesCountAndKeepSessionAlive) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));

  tracker.notePageAdvance(T0 + 30);
  tracker.notePageAdvance(T0 + 60);

  SessionRecord record;
  EXPECT_FALSE(tracker.tick(T0 + 140, record));  // 80 s since last turn
  ASSERT_TRUE(tracker.tick(T0 + 150, record));   // 90 s since last turn
  EXPECT_EQ(record.pagesAdvanced, 2u);
  EXPECT_EQ(record.endSec, T0 + 60 + IDLE_TIMEOUT_SEC);
}

TEST(ReadingSessionTracker, FinishReturnsCountedPages) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));
  tracker.notePageAdvance(T0 + 10);
  tracker.notePageAdvance(T0 + 20);
  tracker.notePageAdvance(T0 + 30);

  SessionRecord record;
  ASSERT_TRUE(tracker.finish(T0 + 40, record));
  EXPECT_EQ(record.pagesAdvanced, 3u);
}

TEST(ReadingSessionTracker, PageTurnAfterIdleStartsNewSession) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));

  SessionRecord first;
  ASSERT_TRUE(tracker.tick(T0 + IDLE_TIMEOUT_SEC, first));
  EXPECT_FALSE(tracker.active());

  tracker.notePageAdvance(T0 + 600);
  EXPECT_TRUE(tracker.active());
  SessionRecord second;
  ASSERT_TRUE(tracker.finish(T0 + 605, second));
  EXPECT_EQ(second.startSec, T0 + 600);
  EXPECT_EQ(second.pagesAdvanced, 1u);
}

TEST(ReadingSessionTracker, FinishTwiceWritesOnce) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));

  SessionRecord first;
  ASSERT_TRUE(tracker.finish(T0 + 10, first));
  SessionRecord second;
  EXPECT_FALSE(tracker.finish(T0 + 20, second));
}

TEST(ReadingSessionTracker, BackwardClockStepDoesNotInflateOrReject) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));

  tracker.noteActivity(T0 - 5);  // SNTP correction
  SessionRecord record;
  ASSERT_TRUE(tracker.finish(T0 + 10, record));
  EXPECT_EQ(record.startSec, T0 - 5);
  EXPECT_EQ(record.endSec, T0 + 10);
  EXPECT_TRUE(record.valid());
}

TEST(ReadingSessionTracker, InsaneClockStepDoesNotCloseOrRebaseSession) {
  SessionTracker tracker;
  ASSERT_TRUE(tracker.start(T0));

  SessionRecord record;
  EXPECT_FALSE(tracker.tick(MIN_SANE_EPOCH_SEC - 1, record));
  EXPECT_TRUE(tracker.active());

  // Once the clock is sane again the session still has its original bounds.
  ASSERT_TRUE(tracker.finish(T0 + 10, record));
  EXPECT_EQ(record.startSec, T0);
  EXPECT_EQ(record.endSec, T0 + 10);
}

TEST(ReadingSessionTracker, UnsyncedStartThenSyncedPageTurnStartsSession) {
  SessionTracker tracker;
  EXPECT_FALSE(tracker.start(MIN_SANE_EPOCH_SEC - 100));

  // Input while the clock is still unsynced must not create a bogus session.
  tracker.noteActivity(MIN_SANE_EPOCH_SEC - 50);
  EXPECT_FALSE(tracker.active());

  tracker.notePageAdvance(T0);
  EXPECT_TRUE(tracker.active());
  SessionRecord record;
  ASSERT_TRUE(tracker.finish(T0 + 5, record));
  EXPECT_EQ(record.startSec, T0);
  EXPECT_EQ(record.pagesAdvanced, 1u);
}
