#pragma once

#include <Arduino.h>

// SHT30 temperature/humidity sensor HAL (OnePage V3.1, I2C 0x44).
//
// The sensor shares the I2C bus with the PCF8563 RTC (GPIO16/17), which
// HalPowerManager::begin() initialises. begin() is optional: read() returns
// false until a valid measurement has been seen, so callers can treat the
// sensor as optional hardware.
class HalEnvSensor {
 public:
  // Probe the sensor once. Returns true if a valid measurement was read.
  bool begin();

  bool isAvailable() const { return available; }

  // Single-shot measurement. Returns false on I2C/CRC error.
  bool read(float& temperatureC, float& humidityPercent);

  static HalEnvSensor& getInstance() { return instance; }

 private:
  HalEnvSensor() = default;
  HalEnvSensor(const HalEnvSensor&) = delete;
  HalEnvSensor& operator=(const HalEnvSensor&) = delete;

  static HalEnvSensor instance;

  bool available = false;
};

#define ENV_SENSOR HalEnvSensor::getInstance()
