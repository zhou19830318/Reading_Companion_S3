#include "HalAccelSensor.h"

#include <Logging.h>
#include <Wire.h>

#include <cmath>

#include "HalGPIO.h"

HalAccelSensor HalAccelSensor::instance;

#if defined(ONEPAGE_S3)

namespace {
constexpr uint8_t REG_RANGE = 0x0F;
constexpr uint8_t REG_ODR = 0x10;
constexpr uint8_t REG_POWER_MODE = 0x11;
constexpr uint8_t REG_DATA_START = 0x02;

constexpr uint8_t RANGE_2G = 0x00;
constexpr uint8_t ODR_62_5_HZ = 0x06;
constexpr uint8_t POWER_MODE_NORMAL = 0x00;
constexpr uint8_t POWER_MODE_SUSPEND = 0xC0;

constexpr float LSB_PER_G = 1024.0f;

bool writeReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readReg(uint8_t addr, uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  value = Wire.read();
  return true;
}
}  // namespace

bool HalAccelSensor::begin() {
  available = false;

  const uint8_t candidates[] = {I2C_ADDR_MSA311, I2C_ADDR_MSA311_ALT};
  for (const uint8_t candidate : candidates) {
    uint8_t partId = 0;
    if (readReg(candidate, MSA311_PARTID_REG, partId) && partId == MSA311_PARTID_VALUE) {
      address = candidate;
      available = true;
      break;
    }
  }

  if (!available) {
    LOG_INF("ACC", "MSA311 accelerometer not found");
    return false;
  }

  if (!writeReg(address, REG_RANGE, RANGE_2G) || !writeReg(address, REG_ODR, ODR_62_5_HZ) ||
      !writeReg(address, REG_POWER_MODE, POWER_MODE_NORMAL)) {
    LOG_ERR("ACC", "MSA311 configuration failed at 0x%02X", address);
    available = false;
    return false;
  }

  float x = 0;
  float y = 0;
  float z = 0;
  if (!read(x, y, z)) {
    LOG_ERR("ACC", "MSA311 first sample failed at 0x%02X", address);
    available = false;
    return false;
  }

  LOG_INF("ACC", "MSA311 found at 0x%02X: X=%d mg Y=%d mg Z=%d mg", address, static_cast<int>(lroundf(x * 1000.0f)),
          static_cast<int>(lroundf(y * 1000.0f)), static_cast<int>(lroundf(z * 1000.0f)));
  return true;
}

bool HalAccelSensor::read(float& x, float& y, float& z) {
  if (!available) {
    return false;
  }

  Wire.beginTransmission(address);
  Wire.write(REG_DATA_START);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(address, static_cast<uint8_t>(6)) != 6) {
    return false;
  }

  uint8_t data[6];
  for (uint8_t& byte : data) {
    byte = Wire.read();
  }

  const int16_t rawX = static_cast<int16_t>((static_cast<uint16_t>(data[1]) << 8) | data[0]) >> 4;
  const int16_t rawY = static_cast<int16_t>((static_cast<uint16_t>(data[3]) << 8) | data[2]) >> 4;
  const int16_t rawZ = static_cast<int16_t>((static_cast<uint16_t>(data[5]) << 8) | data[4]) >> 4;

  x = static_cast<float>(rawX) / LSB_PER_G;
  y = static_cast<float>(rawY) / LSB_PER_G;
  z = static_cast<float>(rawZ) / LSB_PER_G;
  return true;
}

void HalAccelSensor::deepSleep() {
  if (available) {
    writeReg(address, REG_POWER_MODE, POWER_MODE_SUSPEND);
  }
}

#else  // !ONEPAGE_S3

bool HalAccelSensor::begin() {
  available = false;
  return false;
}

bool HalAccelSensor::read(float&, float&, float&) { return false; }

void HalAccelSensor::deepSleep() {}

#endif  // ONEPAGE_S3
