#pragma once

#include <Arduino.h>

// Tilt page-turn facade.
//
// Two boards, two very different sensors, one behaviour: X3 has a QMI8658
// gyroscope (angular rate), OnePage V3.1 has an MSA311 accelerometer and no
// gyroscope at all. HalTiltSensor speaks "degrees per second" and only ever
// starts on X3 (its begin() returns immediately elsewhere), so letting the
// reader ask the sensor directly would mean duplicating the settings check and
// the event bookkeeping at every call site. This class is that one place.
//
//   X3  : every call is forwarded to the existing gyro implementation, so its
//         behaviour is unchanged.
//   S3  : the accelerometer is polled and the same event shape is produced by
//         differencing the gravity-derived tilt angle (a flick is a fast
//         rotation, which an accelerometer sees as the gravity vector swinging
//         across its axes).
//
// The event API mirrors HalTiltSensor exactly so the reader and the main loop
// only change the symbol they talk to.
class HalTiltGesture {
 public:
  static HalTiltGesture& getInstance() { return instance; }

  // Probe the sensor. Safe to call on targets without one.
  void begin();

  bool isAvailable() const;

  // Poll once per main-loop iteration; internally throttled. `mode` is
  // CrossPointTiltPageTurn (TILT_OFF disables and parks the sensor), `orientation`
  // is CrossPointOrientation, `inReader` gates sampling to the reader.
  void update(uint8_t mode, uint8_t orientation, bool inReader);

  // Consumed on read.
  bool wasTiltedForward();
  bool wasTiltedBack();

  // Non-consuming: any motion since the last call (auto-sleep timer).
  bool hadActivity();

  // Park the sensor before deep sleep / power off.
  void deepSleep();

  void clearPendingEvents();

 private:
  HalTiltGesture() = default;
  HalTiltGesture(const HalTiltGesture&) = delete;
  HalTiltGesture& operator=(const HalTiltGesture&) = delete;

  static HalTiltGesture instance;

  bool available = false;
  bool forwardEvent = false;
  bool backEvent = false;
  bool hadActivityFlag = false;

#if defined(ONEPAGE_S3)
  // --- accelerometer gesture state (OnePage V3.1) ---
  // The MSA311 runs at 62.5 Hz, so a 50 Hz poll never reuses a sample.
  static constexpr unsigned long POLL_INTERVAL_MS = 20;
  // Readings are discarded for this long after the first poll: the sensor's
  // first samples after configuration are not settled, and a device being
  // picked up would otherwise fire a page turn.
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;
  static constexpr unsigned long COOLDOWN_MS = 600;
  // Rate that must be exceeded to start a gesture. A deliberate flick swings
  // the screen normal 20-40 degrees in ~150 ms (~150-250 deg/s); reading-hand
  // drift stays under ~40 deg/s.
  static constexpr float RATE_TRIGGER_DPS = 170.0f;
  // The gesture must actually turn the device. An accelerometer cannot tell a
  // fast rotation from a sharp linear shake by rate alone, so the total swing
  // has to cross this too.
  static constexpr float ANGLE_TRIGGER_DEG = 22.0f;
  static constexpr float NEUTRAL_RATE_DPS = 45.0f;
  // Rate must stay below neutral this long before the next gesture may start;
  // without it a slow flick triggers twice on its own tail.
  static constexpr unsigned long NEUTRAL_SETTLE_MS = 120;
  // A loop hiccup longer than this invalidates the rate: dt is a divisor.
  static constexpr unsigned long MAX_DT_MS = 120;

  bool havePitch = false;
  bool armed = true;
  bool gesturing = false;
  bool sensorAwake = false;
  float pitch = 0.0f;
  float previousPitch = 0.0f;
  float gestureStartPitch = 0.0f;
  int8_t gestureDirection = 0;
  unsigned long lastPollMs = 0;
  unsigned long lastTriggerMs = 0;
  unsigned long neutralSinceMs = 0;
  unsigned long wakeMs = 0;

  // Tilt angle in degrees for the given reader orientation, given one sample in
  // g. Sign convention: positive is "top edge away from the reader"; the
  // TILT_INVERTED setting flips it for users who prefer the other direction.
  double tiltDegrees(float x, float y, float z, uint8_t orientation) const;
#endif
};

#define TILT HalTiltGesture::getInstance()
