#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  enum class RtcType : uint8_t { None, DS3231, PCF8563 };

  RtcType _type = RtcType::None;
  bool _available = false;
  mutable bool _timeValid = true;  // PCF8563 VL bit: false until the clock is set
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Full calendar date, as the RTC holds it. weekday: 0 = Sunday .. 6 = Saturday
  // (derived from the date rather than read from the RTC, whose weekday
  // convention differs between the DS3231 and the PCF8563). valid is false when
  // no RTC is fitted or the clock lost power and has not been set since.
  // All fields are UTC: the RTC is kept in UTC and callers apply the display
  // offset (SETTINGS.clockUtcOffsetQ) themselves, as formatTime() does.
  struct DateTime {
    uint16_t year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t weekday = 0;
    bool valid = false;
  };
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised)
  void begin();

  // True if an RTC (DS3231 on X3, PCF8563 on OnePage V3.1) is present
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Read year/month/day. Returns false (and leaves the outputs untouched) when
  // the date cannot be trusted: no RTC, or PCF8563 VL set after a power loss.
  bool getDate(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& weekday) const;

  // Date + time in one read. Cheaper than getTime() + getDate(): both share the
  // same I2C transaction on the PCF8563.
  DateTime getDateTime() const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Sync the DS3231 RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // Set the clock from a caller-supplied date and time (manual time setting, or
  // a sync source other than NTP). year/month/day must be a real date.
  // Returns false when no RTC is fitted or the I2C write fails.
  bool setDateTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second = 0);

 private:
  // Writes time and (when known) the calendar date in one burst. year == 0
  // keeps the date registers untouched, which is what the old time-only path
  // did for boards whose date was never set.
  bool writeDateTimeToRTC(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second);
};
