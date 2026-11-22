#include "HalEnvSensor.h"

#include <Logging.h>
#include <Wire.h>

#include "HalGPIO.h"

HalEnvSensor HalEnvSensor::instance;

#if defined(ONEPAGE_S3)

namespace {
constexpr uint8_t SHT30_CMD_MEASURE_HIGH = 0x24;  // single shot, no clock stretching
constexpr uint8_t SHT30_CMD_MEASURE_LOW = 0x00;
constexpr uint8_t SHT30_MEASURE_DELAY_MS = 20;

uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0xFF;
  while (len--) {
    crc ^= *data++;
    for (int i = 0; i < 8; i++) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}
}  // namespace

bool HalEnvSensor::begin() {
  float t = 0;
  float h = 0;
  available = read(t, h);
  if (available) {
    LOG_INF("ENV", "SHT30 found: %d.%d C, %d %%RH", static_cast<int>(t), static_cast<int>(t * 10) % 10,
            static_cast<int>(h));
  } else {
    LOG_INF("ENV", "SHT30 not found");
  }
  return available;
}

bool HalEnvSensor::read(float& temperatureC, float& humidityPercent) {
  Wire.beginTransmission(I2C_ADDR_SHT30);
  Wire.write(SHT30_CMD_MEASURE_HIGH);
  Wire.write(SHT30_CMD_MEASURE_LOW);
  if (Wire.endTransmission() != 0) {
    return false;
  }

  delay(SHT30_MEASURE_DELAY_MS);

  if (Wire.requestFrom(I2C_ADDR_SHT30, static_cast<uint8_t>(6)) < 6) {
    return false;
  }
  uint8_t data[6];
  for (uint8_t i = 0; i < 6; i++) {
    data[i] = Wire.read();
  }
  if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) {
    LOG_ERR("ENV", "SHT30 CRC mismatch");
    return false;
  }

  const uint16_t rawTemp = (static_cast<uint16_t>(data[0]) << 8) | data[1];
  const uint16_t rawHum = (static_cast<uint16_t>(data[3]) << 8) | data[4];
  temperatureC = -45.0f + 175.0f * static_cast<float>(rawTemp) / 65535.0f;
  humidityPercent = 100.0f * static_cast<float>(rawHum) / 65535.0f;
  return true;
}

#else  // !ONEPAGE_S3

bool HalEnvSensor::begin() {
  available = false;
  return false;
}

bool HalEnvSensor::read(float&, float&) { return false; }

#endif  // ONEPAGE_S3
