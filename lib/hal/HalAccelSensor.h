#pragma once

#include <Arduino.h>

// MSA311 three-axis accelerometer HAL (OnePage V3.1 U19, I2C 0x62).
//
// The board has no gyroscope: this is the only motion sensor. It is kept
// separate from HalTiltSensor (QMI8658 gyro on X3) so the tilt page-turn
// feature does not accidentally treat an accelerometer as a gyro.
//
// begin() is optional: read() returns false until a device has been found.
class HalAccelSensor {
 public:
  // Probe and configure the sensor (normal mode, 62.5 Hz, +/-2 g).
  bool begin();

  bool isAvailable() const { return available; }

  // Read one sample in g. Returns false on I2C error or when absent.
  bool read(float& x, float& y, float& z);

  // Put the sensor into suspend mode before deep sleep.
  void deepSleep();

  static HalAccelSensor& getInstance() { return instance; }

 private:
  HalAccelSensor() = default;
  HalAccelSensor(const HalAccelSensor&) = delete;
  HalAccelSensor& operator=(const HalAccelSensor&) = delete;

  static HalAccelSensor instance;

  uint8_t address = 0;
  bool available = false;
};

#define ACCEL HalAccelSensor::getInstance()
