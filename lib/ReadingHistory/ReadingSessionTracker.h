#pragma once

#include <cstdint>

namespace ReadingHistory {

// A session ends after this much inactivity. The reader's only reliable
// "still reading" signal is input (page turns included), so a page dwelled on
// for longer than this is treated as having left the book. 90 s matches the
// v4.0 plan (RH-01); auto-sleep remains the longer, separate timeout.
inline constexpr int64_t IDLE_TIMEOUT_SEC = 90;

// Same floor as ChatLog::MIN_SANE_EPOCH_MS: an ESP32 that never ran SNTP
// reports 1970, which would file every session under 1970-01-20.
inline constexpr int64_t MIN_SANE_EPOCH_SEC = 1600000000LL;

struct SessionRecord {
  int64_t startSec = 0;
  int64_t endSec = 0;
  uint32_t pagesAdvanced = 0;

  bool valid() const { return startSec >= MIN_SANE_EPOCH_SEC && endSec > startSec; }
};

// RH-01 session state machine. Pure integer bookkeeping so the host tests can
// drive it without a clock; callers pass wall-clock seconds. All methods are
// expected to run on the activity/main task.
class SessionTracker {
 public:
  // Begin a session. Returns false (and stays inactive) while the clock is
  // unsynced; a later noteActivity() retries once the clock is sane.
  bool start(const int64_t nowSec) {
    if (nowSec < MIN_SANE_EPOCH_SEC) {
      active_ = false;
      return false;
    }
    active_ = true;
    startSec_ = nowSec;
    lastActivitySec_ = nowSec;
    pagesAdvanced_ = 0;
    return true;
  }

  bool active() const { return active_; }

  // Any user input keeps an active session alive. Input alone does not start a
  // new session after an idle close: opening a menu or leaving the book should
  // not fabricate a one-second session. The next forward page turn does start
  // one (see notePageAdvance), which is the actual reading signal.
  void noteActivity(const int64_t nowSec) {
    if (!active_) return;
    if (nowSec < MIN_SANE_EPOCH_SEC) return;
    // A backward clock step (SNTP correction) must not look like a huge idle
    // gap or produce a negative duration; rebase the activity marker.
    if (nowSec < lastActivitySec_) {
      lastActivitySec_ = nowSec;
      if (startSec_ > nowSec) startSec_ = nowSec;
      return;
    }
    lastActivitySec_ = nowSec;
  }

  // A page turn that moved the reader forward. Counts one page and counts as
  // activity (read-aloud's automatic turns keep the session alive too). A turn
  // arriving after the idle close starts a fresh session, so TTS auto-turns on
  // a long page never fall into a permanently sessionless state.
  void notePageAdvance(const int64_t nowSec) {
    if (!active_) {
      if (start(nowSec)) ++pagesAdvanced_;
      return;
    }
    if (nowSec < MIN_SANE_EPOCH_SEC) return;
    if (nowSec > lastActivitySec_) lastActivitySec_ = nowSec;
    ++pagesAdvanced_;
  }

  // Close the session when it has been idle for IDLE_TIMEOUT_SEC. The recorded
  // end is the idle boundary (last activity + timeout), not the moment the
  // caller noticed, so a slow main loop cannot inflate the duration.
  bool tick(const int64_t nowSec, SessionRecord& out) {
    if (!active_) return false;
    // A clock that fell back to 1970 (RTC reset without a reboot) must not
    // rebase the session: the next sane finish() would then record a duration
    // of decades. Ignore the tick and keep the last sane activity marker.
    if (nowSec < MIN_SANE_EPOCH_SEC) return false;
    if (nowSec < lastActivitySec_) {
      lastActivitySec_ = nowSec;
      if (startSec_ > nowSec) startSec_ = nowSec;
      return false;
    }
    if (nowSec - lastActivitySec_ < IDLE_TIMEOUT_SEC) return false;
    out = makeRecord(lastActivitySec_ + IDLE_TIMEOUT_SEC);
    active_ = false;
    return out.valid();
  }

  // Close at an explicit boundary: leaving the reader or pushing another
  // activity on top of it. Returns false for a zero-length session.
  bool finish(const int64_t nowSec, SessionRecord& out) {
    if (!active_) return false;
    active_ = false;
    out = makeRecord(nowSec);
    return out.valid();
  }

 private:
  SessionRecord makeRecord(const int64_t endSec) const {
    SessionRecord record;
    record.startSec = startSec_;
    record.endSec = endSec;
    record.pagesAdvanced = pagesAdvanced_;
    return record;
  }

  int64_t startSec_ = 0;
  int64_t lastActivitySec_ = 0;
  uint32_t pagesAdvanced_ = 0;
  bool active_ = false;
};

}  // namespace ReadingHistory
