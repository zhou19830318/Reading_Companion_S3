#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)
//
// PCF8563 (OnePage V3.1) register layout (BCD encoded):
//   0x00: Control_1 (bit 5 = STOP)
//   0x01: Control_2
//   0x02: Seconds (bit 7 = VL = clock integrity not guaranteed)
//   0x03: Minutes
//   0x04: Hours (bit 5 = 12/24 mode; we keep it in 24h mode)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

static bool bcdValid(uint8_t bcd) { return (bcd & 0x0F) <= 9 && ((bcd >> 4) & 0x0F) <= 9; }

// 0 = Sunday .. 6 = Saturday (Sakamoto). Derived from the date because the two
// RTCs number weekday registers differently and neither matches a civil index
// we would want to render.
static uint8_t weekdayFromYmd(uint16_t year, uint8_t month, uint8_t day) {
  static constexpr uint8_t offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 1 || month > 12) return 0;
  uint16_t y = year;
  if (month < 3) y -= 1;
  return static_cast<uint8_t>((y + y / 4 - y / 100 + y / 400 + offsets[month - 1] + day) % 7);
}

static uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static constexpr uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return days[month - 1];
}

static bool readI2CReg8(uint8_t addr, uint8_t reg, uint8_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(1), static_cast<uint8_t>(true)) < 1) {
    return false;
  }
  *outValue = Wire.read();
  return true;
}

void HalClock::begin() {
#if defined(ONEPAGE_S3)
  // I2C is initialised by HalPowerManager::begin() on V3.1.
  _type = RtcType::PCF8563;
  uint8_t sec = 0;
  if (!readI2CReg8(I2C_ADDR_PCF8563, 0x02, &sec)) {
    LOG_INF("CLK", "PCF8563 RTC not found");
    _type = RtcType::None;
    _available = false;
    return;
  }
  _available = true;
  // VL is set after a power loss: the stored time is not trustworthy until the
  // next NTP sync writes it (and clears VL).
  _timeValid = (sec & 0x80) == 0;
  LOG_INF("CLK", "PCF8563 RTC found%s", _timeValid ? "" : " (clock not set)");

  uint8_t h, m;
  getTime(h, m);
  return;
#else
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C is already initialised by HalPowerManager::begin() for X3.
  // Probe the DS3231 by reading the seconds register.
  uint8_t sec = 0;
  if (!readI2CReg8(I2C_ADDR_DS3231, DS3231_SEC_REG, &sec)) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }

  _type = RtcType::DS3231;
  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);
#endif
}

HalClock::DateTime HalClock::getDateTime() const {
  DateTime dt;
  if (!_available) return dt;

  const bool isPcf = (_type == RtcType::PCF8563);
  const uint8_t addr = isPcf ? I2C_ADDR_PCF8563 : I2C_ADDR_DS3231;
  const uint8_t startReg = isPcf ? 0x02 : DS3231_SEC_REG;

  Wire.beginTransmission(addr);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) return dt;
  if (Wire.requestFrom(addr, static_cast<uint8_t>(7)) < 7) return dt;

  const uint8_t rawSec = Wire.read();
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();
  uint8_t rawDay = 0;
  uint8_t rawWeekday = 0;
  uint8_t rawMonth = 0;
  uint8_t rawYear = 0;
  if (isPcf) {
    rawDay = Wire.read();
    rawWeekday = Wire.read();
    rawMonth = Wire.read();
    rawYear = Wire.read();
  } else {
    // DS3231: 0x00 sec, 0x01 min, 0x02 hour, 0x03 weekday, 0x04 date, 0x05 month, 0x06 year.
    rawWeekday = Wire.read();
    rawDay = Wire.read();
    rawMonth = Wire.read();
    rawYear = Wire.read();
  }
  (void)rawWeekday;  // not trusted; weekday is derived from the date instead

  if (isPcf && (rawSec & 0x80)) return dt;  // VL: clock integrity not guaranteed
  if (!bcdValid(rawDay & 0x3F) || !bcdValid(rawMonth & 0x1F) || !bcdValid(rawYear)) return dt;
  if (!bcdValid(static_cast<uint8_t>(rawMin & 0x7F)) || !bcdValid(static_cast<uint8_t>(rawHour & 0x3F))) {
    return dt;
  }

  const uint8_t month = bcdToDec(static_cast<uint8_t>(rawMonth & 0x1F));
  const uint8_t day = bcdToDec(static_cast<uint8_t>(rawDay & 0x3F));
  // Century bit: PCF8563 1 = 2000s, DS3231 1 = 2100s.
  const uint16_t century = (rawMonth & 0x80) ? (isPcf ? 2000 : 2100) : (isPcf ? 1900 : 2000);
  const uint16_t year = static_cast<uint16_t>(century + bcdToDec(rawYear));

  if (month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) return dt;

  // Both RTCs are written in 24h mode (see writeDateTimeToRTC), so bit 6 of the
  // hours register is always clear for us; mask 0x3F accepts either encoding.
  const uint8_t hour = bcdToDec(static_cast<uint8_t>(rawHour & 0x3F));
  const uint8_t minute = bcdToDec(static_cast<uint8_t>(rawMin & 0x7F));
  if (hour > 23 || minute > 59) return dt;

  dt.year = year;
  dt.month = month;
  dt.day = day;
  dt.hour = hour;
  dt.minute = minute;
  dt.weekday = weekdayFromYmd(year, month, day);
  dt.valid = true;
  return dt;
}

bool HalClock::getDate(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& weekday) const {
  const DateTime dt = getDateTime();
  if (!dt.valid) return false;
  year = dt.year;
  month = dt.month;
  day = dt.day;
  weekday = dt.weekday;
  return true;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available || !_timeValid) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  const bool isPcf = (_type == RtcType::PCF8563);
  const uint8_t addr = isPcf ? I2C_ADDR_PCF8563 : I2C_ADDR_DS3231;
  const uint8_t startReg = isPcf ? 0x02 : DS3231_SEC_REG;

  // Read 3 bytes: seconds, minutes, hours
  Wire.beginTransmission(addr);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  Wire.requestFrom(addr, (uint8_t)3);
  if (Wire.available() < 3) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  const uint8_t rawSec = Wire.read();
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();

  if (isPcf) {
    if (rawSec & 0x80) {
      // VL became set (RTC lost power): do not report a bogus time.
      _timeValid = false;
      return false;
    }
    _cachedMinute = bcdToDec(rawMin & 0x7F);
    _cachedHour = bcdToDec(rawHour & 0x3F);  // we always write 24h mode
  } else {
    _cachedMinute = bcdToDec(rawMin & 0x7F);
    // Handle 12/24h mode: bit 6 high = 12h mode
    if (rawHour & 0x40) {
      // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
      uint8_t h12 = bcdToDec(rawHour & 0x1F);
      bool pm = rawHour & 0x20;
      if (h12 == 12) h12 = 0;
      _cachedHour = pm ? (h12 + 12) : h12;
    } else {
      // 24h mode: bits 5-0 = hours (0-23)
      _cachedHour = bcdToDec(rawHour & 0x3F);
    }
  }
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::setDateTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
  if (!_available || hour > 23 || minute > 59 || second > 59) return false;
  if (month < 1 || month > 12 || day < 1 || day > daysInMonth(year, month)) {
    LOG_ERR("CLK", "setDateTime: bad date %04u-%02u-%02u", year, month, day);
    return false;
  }
  return writeDateTimeToRTC(year, month, day, hour, minute, second);
}

bool HalClock::writeDateTimeToRTC(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                                  uint8_t second) {
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);

  const bool withDate = year >= 2000 && month >= 1 && month <= 12;
#if defined(ONEPAGE_S3)
  Wire.beginTransmission(I2C_ADDR_PCF8563);
  Wire.write(0x02);              // Start at the seconds register
  Wire.write(decToBcd(second));  // clearing bit 7 also clears VL
  Wire.write(decToBcd(minute));
  Wire.write(decToBcd(hour));  // 24h mode (bit 5 = 0)
  if (withDate) {
    Wire.write(decToBcd(day));
    Wire.write(decToBcd(weekdayFromYmd(year, month, day)));  // PCF8563 weekday is 0-6
    Wire.write(static_cast<uint8_t>(decToBcd(month) | (year >= 2000 ? 0x80 : 0x00)));
    Wire.write(decToBcd(static_cast<uint8_t>(year % 100)));
  }
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to PCF8563");
    return false;
  }
  // Normal mode, clock running (STOP = 0, TESTC = 0).
  Wire.beginTransmission(I2C_ADDR_PCF8563);
  Wire.write(0x00);
  Wire.write(0x00);
  Wire.endTransmission();
  _timeValid = true;
#else
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);    // Start at register 0x00
  Wire.write(decToBcd(second));  // 0x00: Seconds
  Wire.write(decToBcd(minute));  // 0x01: Minutes
  Wire.write(decToBcd(hour));    // 0x02: Hours (24h mode, bit 6 = 0)
  if (withDate) {
    Wire.write(static_cast<uint8_t>(weekdayFromYmd(year, month, day) + 1));  // DS3231: 1-7, 1 = Sunday
    Wire.write(decToBcd(day));
    // Century bit: 1 = 2100s. 1900s is out of range for a device clock.
    Wire.write(static_cast<uint8_t>(decToBcd(month) | (year >= 2100 ? 0x80 : 0x00)));
    Wire.write(decToBcd(static_cast<uint8_t>(year % 100)));
  }
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }
#endif

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  // aliyun first: pool.ntp.org is frequently unroutable from mainland China,
  // which is where most of these devices are. configTzTime takes up to three
  // servers and SNTP tries them in order.
  configTzTime("UTC0", "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      const uint16_t year = static_cast<uint16_t>(timeinfo.tm_year + 1900);
      const uint8_t month = static_cast<uint8_t>(timeinfo.tm_mon + 1);
      if (writeDateTimeToRTC(year, month, static_cast<uint8_t>(timeinfo.tm_mday), timeinfo.tm_hour, timeinfo.tm_min,
                             timeinfo.tm_sec)) {
        LOG_INF("CLK", "RTC set to %04u-%02u-%02u %02d:%02d:%02d UTC", year, month, timeinfo.tm_mday, timeinfo.tm_hour,
                timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
