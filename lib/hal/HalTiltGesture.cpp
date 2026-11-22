#include "HalTiltGesture.h"

#include <Logging.h>

#include <cmath>

#include "HalAccelSensor.h"
#include "HalTiltSensor.h"

HalTiltGesture HalTiltGesture::instance;

#if defined(ONEPAGE_S3)

namespace {

// The screen normal is +Z, the screen's "up" edge is +Y and its left-right
// axis is +X on this board: with the device flat on a table the accelerometer
// reads X~20 mg, Y~125 mg, Z~1008 mg (CMD:IMU on the bench units).
//
// A page-turn flick rotates the device about the screen's left-right axis, so
// the angle to difference is the one between gravity and the screen normal,
// measured in the plane spanned by the screen-up axis and the normal. Which
// physical axis is "screen up" depends on how the reader is rotated.
double tiltDegreesFor(float x, float y, float z, uint8_t orientation) {
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      return std::atan2(static_cast<double>(y), static_cast<double>(z)) * 57.29577951308232;
    case CrossPointOrientation::INVERTED:
      return std::atan2(-static_cast<double>(y), static_cast<double>(z)) * 57.29577951308232;
    case CrossPointOrientation::LANDSCAPE_CW:
      return std::atan2(static_cast<double>(x), static_cast<double>(z)) * 57.29577951308232;
    case CrossPointOrientation::LANDSCAPE_CCW:
      return std::atan2(-static_cast<double>(x), static_cast<double>(z)) * 57.29577951308232;
    default:
      return std::atan2(static_cast<double>(y), static_cast<double>(z)) * 57.29577951308232;
  }
}

// Shortest signed difference between two angles, so a sample crossing the
// +/-180 wrap is not read as a 360 degree flick.
double angleDelta(double from, double to) {
  double delta = to - from;
  while (delta > 180.0) delta -= 360.0;
  while (delta < -180.0) delta += 360.0;
  return delta;
}

}  // namespace

double HalTiltGesture::tiltDegrees(float x, float y, float z, uint8_t orientation) const {
  return tiltDegreesFor(x, y, z, orientation);
}

void HalTiltGesture::begin() {
  available = ACCEL.begin();
  if (!available) {
    LOG_INF("TILT", "no accelerometer; tilt page turn unavailable");
    return;
  }
  sensorAwake = true;
  wakeMs = millis();
  lastPollMs = wakeMs;
  neutralSinceMs = wakeMs;
  havePitch = false;
  armed = true;
  gesturing = false;
  LOG_INF("TILT", "accelerometer tilt gestures enabled");
}

bool HalTiltGesture::isAvailable() const { return available; }

void HalTiltGesture::update(const uint8_t mode, const uint8_t orientation, const bool inReader) {
  if (!available) {
    return;
  }
  // Park the sensor whenever the feature is off or the reader is not on
  // screen: the MSA311 at 62.5 Hz is a measurable idle drain, and no other
  // activity consumes these events.
  const bool wantSampling = mode != CrossPointTiltPageTurn::TILT_OFF && inReader;
  if (!wantSampling) {
    if (gesturing) {
      gesturing = false;
      armed = true;
    }
    havePitch = false;
    return;
  }

  const unsigned long now = millis();
  if (now - wakeMs < WAKE_STABILIZE_MS) {
    return;
  }
  if (now - lastPollMs < POLL_INTERVAL_MS) {
    return;
  }
  const unsigned long dtMs = now - lastPollMs;
  lastPollMs = now;
  if (dtMs > MAX_DT_MS) {
    havePitch = false;  // rate would be meaningless across a stall
    return;
  }

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!ACCEL.read(x, y, z)) {
    return;
  }

  float angle = static_cast<float>(tiltDegrees(x, y, z, orientation));
  if (mode == CrossPointTiltPageTurn::TILT_INVERTED) {
    angle = -angle;
  }

  if (!havePitch) {
    previousPitch = angle;
    havePitch = true;
    return;
  }

  const float delta = static_cast<float>(angleDelta(previousPitch, angle));
  previousPitch = angle;
  const float rateDps = delta * 1000.0f / static_cast<float>(dtMs);
  const float absRate = std::fabs(rateDps);
  pitch = angle;

  if (absRate > NEUTRAL_RATE_DPS) {
    hadActivityFlag = true;
    neutralSinceMs = now;
  }

  if (gesturing) {
    const float travelled = (pitch - gestureStartPitch) * static_cast<float>(gestureDirection);
    if (travelled >= ANGLE_TRIGGER_DEG) {
      if (gestureDirection > 0) {
        forwardEvent = true;
      } else {
        backEvent = true;
      }
      hadActivityFlag = true;
      gesturing = false;
      armed = false;
      lastTriggerMs = now;
      neutralSinceMs = now;
      LOG_INF("TILT", "gesture %s (%.0f deg, %.0f dps)", gestureDirection > 0 ? "forward" : "back",
              static_cast<double>(travelled), static_cast<double>(absRate));
    } else if (absRate < NEUTRAL_RATE_DPS) {
      // Motion stopped before covering the angle: an aborted flick, not a page
      // turn. Re-arm immediately rather than swallowing the next real gesture.
      gesturing = false;
      LOG_DBG("TILT", "aborted gesture (%.0f deg)", static_cast<double>(travelled));
    }
    return;
  }

  if (!armed) {
    if (absRate < NEUTRAL_RATE_DPS && now - neutralSinceMs >= NEUTRAL_SETTLE_MS) {
      armed = true;
    }
    return;
  }

  if (now - lastTriggerMs >= COOLDOWN_MS && absRate >= RATE_TRIGGER_DPS) {
    gesturing = true;
    gestureStartPitch = pitch;
    gestureDirection = rateDps > 0.0f ? 1 : -1;
  }
}

bool HalTiltGesture::wasTiltedForward() {
  const bool value = forwardEvent;
  forwardEvent = false;
  return value;
}

bool HalTiltGesture::wasTiltedBack() {
  const bool value = backEvent;
  backEvent = false;
  return value;
}

bool HalTiltGesture::hadActivity() {
  const bool value = hadActivityFlag;
  hadActivityFlag = false;
  return value;
}

void HalTiltGesture::deepSleep() {
  ACCEL.deepSleep();
  sensorAwake = false;
  clearPendingEvents();
  gesturing = false;
  havePitch = false;
}

void HalTiltGesture::clearPendingEvents() {
  forwardEvent = false;
  backEvent = false;
  hadActivityFlag = false;
}

#else  // !ONEPAGE_S3 — X3/X4 keep the gyroscope implementation untouched

void HalTiltGesture::begin() {
  halTiltSensor.begin();
  available = halTiltSensor.isAvailable();
}

bool HalTiltGesture::isAvailable() const { return halTiltSensor.isAvailable(); }

void HalTiltGesture::update(const uint8_t mode, const uint8_t orientation, const bool inReader) {
  halTiltSensor.update(mode, orientation, inReader);
}

bool HalTiltGesture::wasTiltedForward() { return halTiltSensor.wasTiltedForward(); }

bool HalTiltGesture::wasTiltedBack() { return halTiltSensor.wasTiltedBack(); }

bool HalTiltGesture::hadActivity() { return halTiltSensor.hadActivity(); }

void HalTiltGesture::deepSleep() { halTiltSensor.deepSleep(); }

void HalTiltGesture::clearPendingEvents() { halTiltSensor.clearPendingEvents(); }

#endif  // ONEPAGE_S3
