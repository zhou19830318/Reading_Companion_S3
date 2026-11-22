#pragma once

#include <WeatherClient.h>

#include <cstdint>

#include "activities/Activity.h"

struct Rect;

// 万年历: a single portrait page modelled on the traditional 老黄历 day sheet
// (month grid on top, the day's almanac below). The day block leads with a large
// day number flanked by prev/next-day arrows, then the lunar date + zodiac, the
// three 干支 pillars, 宜/忌, the rule-derived day fields (冲煞/五行/建除/财神/喜神),
// the surrounding solar terms, and finally the live readings (SHT30 indoors plus
// the cached forecast).
//
// Data sources, in the order they are trusted: the PCF8563/DS3231 RTC for the
// date, the relay for weather (cached on SD so the screen works offline), and the
// SHT30 for the room. Nothing here is inferred: with no RTC the page asks for a
// clock sync instead of inventing a date, and Confirm actually runs that sync.
class CalendarActivity final : public Activity {
 public:
  CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calendar", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return weatherPending_ || weatherLoading_ || clockPending_ || clockSyncing_; }

 private:
  uint16_t year_ = 0;
  uint8_t month_ = 0;
  uint8_t day_ = 0;
  uint8_t weekday_ = 0;  // 0 = Sunday
  bool hasDate_ = false;

  // Selected day, independent of today so paging away and back is stable.
  uint16_t viewYear_ = 0;
  uint8_t viewMonth_ = 1;
  uint8_t viewDay_ = 1;

  Weather::Client weatherClient_;
  Weather::Report weather_;
  bool weatherLoading_ = false;
  bool weatherPending_ = false;  // set once the activity is current again (Wi-Fi result)
  Weather::Client::Result weatherError_ = Weather::Client::Result::Ok;

  bool clockSyncing_ = false;
  bool clockPending_ = false;

  bool hasEnv_ = false;
  float envTemp_ = 0.0f;
  float envHumidity_ = 0.0f;
  unsigned long envReadMs_ = 0;

  void refreshDate();
  void refreshEnv(bool force);
  void startWeatherRefresh();
  void runWeatherRefresh();
  void startClockSync();
  void runClockSync();
  void openWifiPicker();
  bool selectedIsToday() const;
  void stepDay(int delta);
  void stepMonth(int delta);

  void drawToolbar(const Rect& safe) const;
  void drawGrid(const Rect& area, int cellH, int rows) const;
  void drawDayBlock(const Rect& area) const;
  void drawWeatherBlock(int x, int y, int width) const;
};