#include "CalendarActivity.h"

#include <Almanac.h>
#include <CloudConfig.h>
#include <HalClock.h>
#include <HalEnvSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr unsigned long ENV_REFRESH_MS = 30000;
constexpr int TOOLBAR_HEIGHT = 26;
constexpr int DAY_NUMBER_SCALE = 2;
constexpr int DAY_NUMBER_BOX_H = 48;
constexpr int FORECAST_DAYS = 2;

// The day number wants a glyph much taller than the biggest UI face, but a
// one-off 40 px font would cost hundreds of KB of flash. Scale the built-in
// 18 px digits instead; only the day number uses this path. Glyphs are centred
// on their ink, not on the font ascender.
void drawScaledText(const GfxRenderer& renderer, const int fontId, const int x, const int boxY, const int boxHeight,
                    const char* text, const int scale, const bool black, const EpdFontFamily::Style style) {
  if (text == nullptr || *text == '\0') return;
  const auto& fonts = renderer.getFontMap();
  const auto fontIt = fonts.find(fontId);
  if (fontIt == fonts.end()) return;
  const EpdFontFamily& font = fontIt->second;
  const EpdFontData* fontData = font.getData(style);
  if (fontData == nullptr) return;

  int maxAscent = 0;
  int maxDescent = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    const EpdGlyph* glyph = font.getGlyph(static_cast<uint32_t>(*p), style);
    if (glyph == nullptr) continue;
    if (glyph->top > maxAscent) maxAscent = glyph->top;
    const int descent = static_cast<int>(glyph->height) - glyph->top;
    if (descent > maxDescent) maxDescent = descent;
  }
  const int inkHeight = (maxAscent + maxDescent) * scale;
  const int baseline = boxY + (boxHeight - inkHeight) / 2 + maxAscent * scale;
  int cursorX = x;
  for (const char* p = text; *p != '\0'; ++p) {
    const EpdGlyph* glyph = font.getGlyph(static_cast<uint32_t>(*p), style);
    if (glyph == nullptr) continue;
    const uint8_t* bitmap = renderer.getGlyphBitmap(fontData, glyph);
    if (bitmap != nullptr) {
      const bool is2Bit = renderer.getGlyphIs2Bit(fontData, glyph);
      const int originX = cursorX + glyph->left * scale;
      const int originY = baseline - glyph->top * scale;
      for (int gy = 0; gy < glyph->height; ++gy) {
        for (int gx = 0; gx < glyph->width; ++gx) {
          const int pos = gy * glyph->width + gx;
          bool ink = false;
          if (is2Bit) {
            ink = ((bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3) != 0;
          } else {
            ink = ((bitmap[pos >> 3] >> (7 - (pos & 7))) & 0x1) != 0;
          }
          if (ink) {
            renderer.fillRect(originX + gx * scale, originY + gy * scale, scale, scale, black);
          }
        }
      }
    }
    cursorX += fp4::toPixel(glyph->advanceX) * scale;
  }
}

uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return kDays[month - 1];
}

// 0 = Sunday (Sakamoto), matching HalClock::DateTime::weekday.
uint8_t weekdayOf(uint16_t year, uint8_t month, uint8_t day) {
  static constexpr uint8_t kOffsets[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 1 || month > 12) return 0;
  uint16_t y = year;
  if (month < 3) y -= 1;
  return static_cast<uint8_t>((y + y / 4 - y / 100 + y / 400 + kOffsets[month - 1] + day) % 7);
}

const char* weekdayLabel(uint8_t weekday) {
  switch (weekday) {
    case 0:
      return tr(STR_WD_SUN);
    case 1:
      return tr(STR_WD_MON);
    case 2:
      return tr(STR_WD_TUE);
    case 3:
      return tr(STR_WD_WED);
    case 4:
      return tr(STR_WD_THU);
    case 5:
      return tr(STR_WD_FRI);
    default:
      return tr(STR_WD_SAT);
  }
}

const char* weatherLabel(uint8_t category) {
  switch (category) {
    case 0:
      return tr(STR_WX_CLEAR);
    case 1:
      return tr(STR_WX_PARTLY);
    case 3:
      return tr(STR_WX_FOG);
    case 4:
      return tr(STR_WX_DRIZZLE);
    case 5:
      return tr(STR_WX_RAIN);
    case 6:
      return tr(STR_WX_SNOW);
    case 7:
      return tr(STR_WX_THUNDER);
    default:
      return tr(STR_WX_OVERCAST);
  }
}

bool isChineseUi() {
  const Language language = I18N.getLanguage();
  return language == Language::ZH_HANS || language == Language::ZH_HANT;
}

const char* weatherErrorLabel(Weather::Client::Result result) {
  switch (result) {
    case Weather::Client::Result::Config:
      return tr(STR_SERVER_NOT_CONFIGURED);
    case Weather::Client::Result::Auth:
      return tr(STR_VOICE_RELAY_AUTH);
    case Weather::Client::Result::Wifi:
      return tr(STR_NEED_WIFI);
    default:
      return tr(STR_MARKET_FAILED);
  }
}

}  // namespace

void CalendarActivity::onEnter() {
  Activity::onEnter();
  refreshDate();
  viewYear_ = year_;
  viewMonth_ = month_ ? month_ : 1;
  viewDay_ = day_ ? day_ : 1;
  // NVS, idempotent. Without this the first weather refresh after a reboot
  // reports Result::Config ("not configured") until some other screen happens
  // to load the relay settings.
  CLOUD_CFG.load();
  Weather::Client::loadCache(weather_);
  refreshEnv(/*force=*/true);
  // A clock that lost power can be repaired right here if the link is already
  // up; otherwise the page shows the sync hint and Confirm opens the picker.
  if (!hasDate_ && WiFi.isConnected()) clockPending_ = true;
  requestUpdate();
}

void CalendarActivity::refreshDate() {
  // The RTC is kept in UTC; every date on this screen is local, so shift by the
  // user's offset before deriving the lunar date / weekday / 干支.
  const HalClock::DateTime utc = halClock.getDateTime();
  hasDate_ = utc.valid;
  if (!hasDate_) return;

  const int offsetMinutes = (static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48) * 15;
  int minutes = static_cast<int>(utc.hour) * 60 + static_cast<int>(utc.minute) + offsetMinutes;
  uint16_t year = utc.year;
  uint8_t month = utc.month;
  uint8_t day = utc.day;
  while (minutes < 0) {
    minutes += 24 * 60;
    if (day > 1) {
      day--;
    } else if (month > 1) {
      month--;
      day = daysInMonth(year, month);
    } else {
      year--;
      month = 12;
      day = 31;
    }
  }
  while (minutes >= 24 * 60) {
    minutes -= 24 * 60;
    if (day < daysInMonth(year, month)) {
      day++;
    } else if (month < 12) {
      month++;
      day = 1;
    } else {
      year++;
      month = 1;
      day = 1;
    }
  }

  year_ = year;
  month_ = month;
  day_ = day;
  weekday_ = weekdayOf(year, month, day);
}

void CalendarActivity::refreshEnv(bool force) {
  const unsigned long now = millis();
  if (!force && envReadMs_ != 0 && now - envReadMs_ < ENV_REFRESH_MS) return;
  envReadMs_ = now;
  if (!ENV_SENSOR.isAvailable()) return;
  float temperature = 0.0f;
  float humidity = 0.0f;
  if (!ENV_SENSOR.read(temperature, humidity)) return;
  // Only repaint for a reading the user could actually see change: an e-ink
  // refresh per 0.1 °C would cost far more than it tells anyone.
  const bool changed = !hasEnv_ || temperature - envTemp_ >= 0.2f || envTemp_ - temperature >= 0.2f ||
                       humidity - envHumidity_ >= 2.0f || envHumidity_ - humidity >= 2.0f;
  envTemp_ = temperature;
  envHumidity_ = humidity;
  hasEnv_ = true;
  if (changed && !force) requestUpdate();
}

bool CalendarActivity::selectedIsToday() const {
  return hasDate_ && viewYear_ == year_ && viewMonth_ == month_ && viewDay_ == day_;
}

void CalendarActivity::stepDay(int delta) {
  int year = viewYear_;
  int month = viewMonth_;
  int day = static_cast<int>(viewDay_) + delta;
  if (day < 1) {
    month--;
    if (month < 1) {
      month = 12;
      year--;
    }
    day = daysInMonth(static_cast<uint16_t>(year), static_cast<uint8_t>(month));
  } else if (day > daysInMonth(static_cast<uint16_t>(year), static_cast<uint8_t>(month))) {
    day = 1;
    month++;
    if (month > 12) {
      month = 1;
      year++;
    }
  }
  if (year < 1900 || year > 2100) return;
  viewYear_ = static_cast<uint16_t>(year);
  viewMonth_ = static_cast<uint8_t>(month);
  viewDay_ = static_cast<uint8_t>(day);
  requestUpdate();
}

void CalendarActivity::stepMonth(int delta) {
  int year = viewYear_;
  int month = static_cast<int>(viewMonth_) + delta;
  if (month < 1) {
    month = 12;
    year--;
  } else if (month > 12) {
    month = 1;
    year++;
  }
  if (year < 1900 || year > 2100) return;
  viewYear_ = static_cast<uint16_t>(year);
  viewMonth_ = static_cast<uint8_t>(month);
  viewDay_ = std::min<uint8_t>(viewDay_, daysInMonth(viewYear_, viewMonth_));
  requestUpdate();
}

void CalendarActivity::loop() {
  refreshEnv(/*force=*/false);

  if (clockPending_) {
    runClockSync();
    return;
  }
  if (weatherPending_) {
    runWeatherRefresh();
    return;
  }

  using Button = MappedInputManager::Button;
  if (mappedInput.wasReleased(Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(Button::Confirm)) {
    if (!hasDate_) {
      startClockSync();
    } else if (selectedIsToday()) {
      startWeatherRefresh();
    } else {
      viewYear_ = year_;
      viewMonth_ = month_;
      viewDay_ = day_;
      requestUpdate();
    }
    return;
  }

  if (!hasDate_) return;

  // Front left/right step the selected day (the arrows around the big number);
  // side up/down step the month. Raw buttons, so the two axes stay independent.
  if (mappedInput.wasReleased(Button::Left)) {
    stepDay(-1);
  } else if (mappedInput.wasReleased(Button::Right)) {
    stepDay(1);
  } else if (mappedInput.wasReleased(Button::Up)) {
    stepMonth(-1);
  } else if (mappedInput.wasReleased(Button::Down)) {
    stepMonth(1);
  }
}

void CalendarActivity::openWifiPicker() {
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("CAL", "OOM: Wi-Fi selection");
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    (void)result;
    // The picker syncs NTP on connect, so a clock that was unset on entry may
    // be usable now. Re-read it and show today instead of the hint.
    const bool hadDate = hasDate_;
    const bool wasToday = selectedIsToday();
    refreshDate();
    if (hasDate_ && (!hadDate || wasToday)) {
      viewYear_ = year_;
      viewMonth_ = month_;
      viewDay_ = day_;
    }
    // Even a cancelled picker re-enters the loop; only queue the fetch when a
    // link actually came up.
    if (WiFi.isConnected()) {
      weatherPending_ = true;
      // The picker brings the link up but does not itself write the RTC; queue
      // the NTP sync so one Confirm press repairs an unset clock end to end.
      if (!hasDate_) clockPending_ = true;
    }
    requestUpdate();
  });
}

void CalendarActivity::startClockSync() {
  if (clockSyncing_ || clockPending_) return;
  if (!WiFi.isConnected()) {
    openWifiPicker();
    return;
  }
  clockPending_ = true;
  requestUpdate();
}

void CalendarActivity::runClockSync() {
  clockPending_ = false;
  {
    RenderLock lock;
    clockSyncing_ = true;
  }
  requestUpdateAndWait();  // show the syncing frame before the blocking SNTP wait
  halClock.syncFromNTP();
  refreshDate();
  if (hasDate_) {
    viewYear_ = year_;
    viewMonth_ = month_;
    viewDay_ = day_;
  }
  {
    RenderLock lock;
    clockSyncing_ = false;
  }
  if (WiFi.isConnected()) weatherPending_ = true;
  requestUpdate();
}

void CalendarActivity::startWeatherRefresh() {
  if (weatherLoading_ || weatherPending_) return;
  if (!WiFi.isConnected()) {
    openWifiPicker();
    return;
  }
  weatherPending_ = true;
  requestUpdate();
}

void CalendarActivity::runWeatherRefresh() {
  weatherPending_ = false;
  if (!weatherClient_.begin()) {
    weatherError_ = Weather::Client::Result::Memory;
    requestUpdate();
    return;
  }
  {
    RenderLock lock;
    weatherLoading_ = true;
  }
  requestUpdateAndWait();  // show the loading frame before the blocking round trip
  Weather::Client::Result result = weatherClient_.fetch(weather_);
  if (result == Weather::Client::Result::Unavailable && WiFi.isConnected()) {
    LOG_DBG("CAL", "weather fetch failed on a live link; retrying once");
    result = weatherClient_.fetch(weather_);
  }
  {
    RenderLock lock;
    weatherLoading_ = false;
    weatherError_ = result;
  }
  if (result == Weather::Client::Result::Ok) {
    Weather::Client::saveCache(weather_);
  } else {
    LOG_ERR("CAL", "weather refresh failed: %d", static_cast<int>(result));
  }
  requestUpdate();
}

void CalendarActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, /*hasFrontButtonHints=*/true,
                                                             /*hasSideButtonHints=*/true);

  drawToolbar(safe);

  const int contentTop = safe.y + metrics.topPadding + TOOLBAR_HEIGHT + 6;
  const int contentBottom = safe.y + safe.height - 2;
  const int lineH10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineH12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int gridHeaderH = lineH10 + 6;
  // A cell holds the solar number (UI_12) above the lunar/term label (UI_10);
  // sizing it from the real line heights is what keeps the two from colliding.
  const int cellH = lineH12 + lineH10 + 2;
  int rows = 6;
  if (hasDate_) {
    const int firstCol = (static_cast<int>(weekdayOf(viewYear_, viewMonth_, 1)) + 6) % 7;
    rows = (firstCol + daysInMonth(viewYear_, viewMonth_) + 6) / 7;
  }
  const int gridH = gridHeaderH + rows * cellH;

  const Rect gridArea{safe.x + 4, contentTop, safe.width - 8, gridH};
  drawGrid(gridArea, cellH, rows);

  int y = contentTop + gridH + 6;

  const Rect dayArea{safe.x + 4, y, safe.width - 8, contentBottom - y};
  drawDayBlock(dayArea);

  // Confirm syncs the clock when there is no date, refreshes when today is on
  // screen, and jumps back to today after paging away.
  const char* confirmLabel = !hasDate_ ? tr(STR_CAL_SYNC) : (selectedIsToday() ? tr(STR_REFRESH) : tr(STR_CAL_TODAY));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_CAL_DAY_PREV), tr(STR_CAL_DAY_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_CAL_MONTH_PREV), tr(STR_CAL_MONTH_NEXT));
  // Every activity owns its panel push. Without this the calendar only ever
  // reached the framebuffer (visible to screenshots) and the e-ink panel kept
  // showing the previous page — reported as "万年历不显示".
  renderer.displayBuffer();
}

void CalendarActivity::drawToolbar(const Rect& safe) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int y = safe.y + metrics.topPadding;
  int x = safe.x + 6;

  const int boxTextY = y + (TOOLBAR_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  const char* actionLabel = !hasDate_ ? tr(STR_CAL_SYNC) : (selectedIsToday() ? tr(STR_REFRESH) : tr(STR_CAL_TODAY));
  renderer.drawText(UI_10_FONT_ID, x, boxTextY, actionLabel, true, EpdFontFamily::BOLD);

  char buf[32];
  if (hasDate_) {
    if (isChineseUi()) {
      snprintf(buf, sizeof(buf), "%04u年%02u月", viewYear_, viewMonth_);
    } else {
      snprintf(buf, sizeof(buf), "%04u-%02u", viewYear_, viewMonth_);
    }
  } else {
    snprintf(buf, sizeof(buf), "----");
  }
  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, y + 1, buf, true, EpdFontFamily::BOLD);

  GUI.drawBatteryRight(renderer, Rect{safe.x + safe.width - 20, y + 7, 15, 12},
                       SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);
}

void CalendarActivity::drawGrid(const Rect& area, int cellH, int rows) const {
  const int lineH10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineH12 = renderer.getLineHeight(UI_12_FONT_ID);

  if (!hasDate_) {
    const char* hint = clockSyncing_ ? tr(STR_SYNCING_TIME) : tr(STR_SET_TIME_HINT);
    UITheme::drawCenteredText(renderer, area, UI_12_FONT_ID, area.y + area.height / 2 - lineH12, hint, true,
                              EpdFontFamily::BOLD);
    return;
  }

  const int headerH = lineH10 + 6;
  const int cellW = area.width / 7;
  char buf[16];

  // Weekday header, Monday first (the layout Chinese calendars use).
  for (int col = 0; col < 7; col++) {
    const uint8_t weekday = static_cast<uint8_t>((col + 1) % 7);  // Monday..Sunday
    const char* label = weekdayLabel(weekday);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    renderer.drawText(UI_10_FONT_ID, area.x + col * cellW + (cellW - textWidth) / 2, area.y + 2, label, true,
                      col >= 5 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  const int bodyTop = area.y + headerH;
  const uint8_t firstWeekday = weekdayOf(viewYear_, viewMonth_, 1);
  const int firstCol = (static_cast<int>(firstWeekday) + 6) % 7;  // Monday = 0
  const int totalDays = daysInMonth(viewYear_, viewMonth_);

  for (int row = 0; row < rows; row++) {
    for (int col = 0; col < 7; col++) {
      const int cellIndex = row * 7 + col;
      const int dayNumber = cellIndex - firstCol + 1;
      const int cx = area.x + col * cellW;
      const int cy = bodyTop + row * cellH;
      const bool isSelected = dayNumber == viewDay_;
      const bool isToday = viewYear_ == year_ && viewMonth_ == month_ && dayNumber == day_;

      if (isSelected) {
        renderer.fillRectDither(cx + 1, cy + 1, cellW - 2, cellH - 2, Color::LightGray);
        renderer.drawRect(cx, cy, cellW, cellH, 2, true);
      }
      if (dayNumber < 1 || dayNumber > totalDays) continue;

      const bool weekend = col >= 5;
      snprintf(buf, sizeof(buf), "%d", dayNumber);
      const int numW = renderer.getTextWidth(UI_12_FONT_ID, buf);
      renderer.drawText(UI_12_FONT_ID, cx + (cellW - numW) / 2, cy + 2, buf, true,
                        (isSelected || isToday || weekend) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      // A small corner dot keeps "today" findable while another day is selected.
      if (isToday && !isSelected) renderer.fillRect(cx + cellW - 6, cy + 3, 3, 3, true);

      Almanac::LunarDate lunar;
      const char* sub = nullptr;
      char subBuf[16] = {};
      const int term = Almanac::solarTermOn(viewYear_, viewMonth_, static_cast<uint8_t>(dayNumber));
      if (term >= 0) {
        snprintf(subBuf, sizeof(subBuf), "%s", Almanac::solarTermName(static_cast<uint8_t>(term)));
        sub = subBuf;
      } else if (Almanac::solarToLunar(viewYear_, viewMonth_, static_cast<uint8_t>(dayNumber), lunar)) {
        sub = lunar.day == 1 ? Almanac::monthName(lunar.month, lunar.leap) : Almanac::dayName(lunar.day);
      }
      if (sub != nullptr) {
        const int subW = renderer.getTextWidth(UI_10_FONT_ID, sub);
        renderer.drawText(UI_10_FONT_ID, cx + (cellW - subW) / 2, cy + 2 + lineH12, sub, true,
                          (term >= 0 || weekend) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
    }
  }
}

void CalendarActivity::drawDayBlock(const Rect& area) const {
  const int lineH10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineH12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int x = area.x;
  const int width = area.width;
  int y = area.y;
  char buf[96];

  if (!hasDate_) {
    if (hasEnv_) {
      snprintf(buf, sizeof(buf), "%s %.1f°C  %u%%", tr(STR_INDOOR), static_cast<double>(envTemp_),
               static_cast<unsigned>(envHumidity_ + 0.5f));
    } else {
      snprintf(buf, sizeof(buf), "%s --", tr(STR_INDOOR));
    }
    renderer.drawText(UI_10_FONT_ID, x, y, buf, true);
    y += lineH10 + 6;
    drawWeatherBlock(x, y, width);
    return;
  }

  const bool chinese = isChineseUi();
  const uint8_t selWeekday = weekdayOf(viewYear_, viewMonth_, viewDay_);

  // Date strip: the reference's coloured header, rendered as an inverted bar.
  if (chinese) {
    snprintf(buf, sizeof(buf), "%04u年%02u月%02u日 %s", viewYear_, viewMonth_, viewDay_, weekdayLabel(selWeekday));
  } else {
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %s", viewYear_, viewMonth_, viewDay_, weekdayLabel(selWeekday));
  }
  constexpr int headerH = 24;
  renderer.fillRect(x, y, width, headerH, true);
  {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, buf);
    renderer.drawText(UI_10_FONT_ID, x + (width - textWidth) / 2, y + (headerH - lineH10) / 2, buf, false,
                      EpdFontFamily::BOLD);
    const int termOnDay = Almanac::solarTermOn(viewYear_, viewMonth_, viewDay_);
    if (chinese && termOnDay >= 0) {
      const char* termName = Almanac::solarTermName(static_cast<uint8_t>(termOnDay));
      const int termWidth = renderer.getTextWidth(UI_10_FONT_ID, termName);
      renderer.drawText(UI_10_FONT_ID, x + width - 6 - termWidth, y + (headerH - lineH10) / 2, termName, false);
    }
  }
  y += headerH + 4;

  // The day number is the focal point, flanked by the prev/next-day arrows.
  const char* leftArrow = "<";
  const char* rightArrow = ">";
  const int arrowY = y + (DAY_NUMBER_BOX_H - lineH12) / 2;
  renderer.drawText(UI_12_FONT_ID, x + 6, arrowY, leftArrow, true, EpdFontFamily::BOLD);
  const int rightArrowW = renderer.getTextWidth(UI_12_FONT_ID, rightArrow, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x + width - 6 - rightArrowW, arrowY, rightArrow, true, EpdFontFamily::BOLD);
  snprintf(buf, sizeof(buf), "%u", viewDay_);
  const int numberWidth = renderer.getTextWidth(NOTOSANS_18_FONT_ID, buf, EpdFontFamily::BOLD) * DAY_NUMBER_SCALE;
  drawScaledText(renderer, NOTOSANS_18_FONT_ID, x + (width - numberWidth) / 2, y, DAY_NUMBER_BOX_H, buf,
                 DAY_NUMBER_SCALE, true, EpdFontFamily::BOLD);
  y += DAY_NUMBER_BOX_H + 2;

  Almanac::LunarDate lunar;
  const bool haveLunar = Almanac::solarToLunar(viewYear_, viewMonth_, viewDay_, lunar);
  if (chinese && haveLunar) {
    snprintf(buf, sizeof(buf), "(%s年) 农历%s%s", Almanac::zodiac(lunar.year),
             Almanac::monthName(lunar.month, lunar.leap), Almanac::dayName(lunar.day));
    UITheme::drawCenteredText(renderer, area, UI_12_FONT_ID, y, buf, true, EpdFontFamily::BOLD);
    y += lineH12 + 2;

    char yearPillar[8];
    char monthPillar[8];
    char dayPillar[8];
    snprintf(yearPillar, sizeof(yearPillar), "%s", Almanac::yearGanZhi(lunar.year));
    Almanac::monthGanZhi(viewYear_, viewMonth_, viewDay_, monthPillar, sizeof(monthPillar));
    Almanac::dayGanZhi(viewYear_, viewMonth_, viewDay_, dayPillar, sizeof(dayPillar));
    snprintf(buf, sizeof(buf), "%s年 %s月 %s日", yearPillar, monthPillar, dayPillar);
    UITheme::drawCenteredText(renderer, area, UI_10_FONT_ID, y, buf, true);
    y += lineH10 + 4;

    auto drawAlmanacList = [&](const char* label, const char* text) {
      const int chip = lineH10;
      renderer.fillRect(x, y, chip, chip, true);
      const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, x + (chip - labelWidth) / 2, y, label, false, EpdFontFamily::BOLD);
      const int textX = x + chip + 6;
      const auto lines = renderer.wrappedText(UI_10_FONT_ID, text, width - chip - 6, 2);
      int textY = y;
      for (const auto& line : lines) {
        renderer.drawText(UI_10_FONT_ID, textX, textY, line.c_str(), true);
        textY += lineH10;
      }
      y += std::max(chip, textY - y) + 2;
    };
    drawAlmanacList(tr(STR_YI), Almanac::yi(viewYear_, viewMonth_, viewDay_));
    drawAlmanacList(tr(STR_JI), Almanac::ji(viewYear_, viewMonth_, viewDay_));

    // Rule-derived day fields, three columns like the reference's detail table.
    char chong[16];
    Almanac::dayChongSha(viewYear_, viewMonth_, viewDay_, chong, sizeof(chong));
    char jianChuBuf[16];
    snprintf(jianChuBuf, sizeof(jianChuBuf), "%s日", Almanac::jianChu(viewYear_, viewMonth_, viewDay_));
    const int col0 = (width - 16) * 40 / 100;
    const int col1 = (width - 16) * 32 / 100;
    const int col2 = (width - 16) - col0 - col1;
    const int x1 = x + col0 + 8;
    const int x2 = x + col0 + col1 + 16;
    auto drawField = [&](const char* label, const char* value, int fx, int fy, int maxW) {
      renderer.drawText(UI_10_FONT_ID, fx, fy, label, true, EpdFontFamily::BOLD);
      const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD) + 4;
      const auto v = renderer.truncatedText(UI_10_FONT_ID, value, maxW - labelW);
      renderer.drawText(UI_10_FONT_ID, fx + labelW, fy, v.c_str(), true);
    };
    y += 2;
    drawField(tr(STR_CHONG_SHA), chong, x, y, col0);
    drawField(tr(STR_WU_XING), Almanac::dayNaYin(viewYear_, viewMonth_, viewDay_), x1, y, col1);
    drawField(tr(STR_JIAN_CHU), jianChuBuf, x2, y, col2);
    y += lineH10 + 2;
    drawField(tr(STR_WEALTH_DIR), Almanac::dayWealthDirection(viewYear_, viewMonth_, viewDay_), x, y, col0);
    drawField(tr(STR_JOY_DIR), Almanac::dayJoyDirection(viewYear_, viewMonth_, viewDay_), x1, y, col1);
    y += lineH10 + 2;

    // Surrounding solar terms on a single line, as the reference lists them.
    uint16_t ty = 0;
    uint8_t tm = 0;
    uint8_t td = 0;
    const int prevTerm = Almanac::prevSolarTerm(viewYear_, viewMonth_, viewDay_, ty, tm, td);
    char prevBuf[48] = {};
    if (prevTerm >= 0) {
      snprintf(prevBuf, sizeof(prevBuf), "%s %s %02u/%02u", tr(STR_TERM_PREV),
               Almanac::solarTermName(static_cast<uint8_t>(prevTerm)), tm, td);
    }
    const int nextTerm = Almanac::nextSolarTerm(viewYear_, viewMonth_, viewDay_, ty, tm, td);
    if (nextTerm >= 0) {
      if (prevBuf[0] != '\0') {
        snprintf(buf, sizeof(buf), "%s · %s %s %02u/%02u", prevBuf, tr(STR_TERM_NEXT),
                 Almanac::solarTermName(static_cast<uint8_t>(nextTerm)), tm, td);
      } else {
        snprintf(buf, sizeof(buf), "%s %s %02u/%02u", tr(STR_TERM_NEXT),
                 Almanac::solarTermName(static_cast<uint8_t>(nextTerm)), tm, td);
      }
      renderer.drawText(UI_10_FONT_ID, x, y, buf, true);
      y += lineH10;
    } else if (prevBuf[0] != '\0') {
      renderer.drawText(UI_10_FONT_ID, x, y, prevBuf, true);
      y += lineH10;
    }
  } else if (haveLunar) {
    snprintf(buf, sizeof(buf), "%s %04u-%02u-%02u", tr(STR_LUNAR), lunar.year, lunar.month, lunar.day);
    renderer.drawText(UI_10_FONT_ID, x, y, buf, true);
    y += lineH10 + 6;
  }

  y += 8;

  if (hasEnv_) {
    snprintf(buf, sizeof(buf), "%s %.1f°C  %u%%", tr(STR_INDOOR), static_cast<double>(envTemp_),
             static_cast<unsigned>(envHumidity_ + 0.5f));
  } else {
    snprintf(buf, sizeof(buf), "%s --", tr(STR_INDOOR));
  }
  renderer.drawText(UI_10_FONT_ID, x, y, buf, true);
  y += lineH10 + 4;

  drawWeatherBlock(x, y, width);
}

void CalendarActivity::drawWeatherBlock(int x, int y, int width) const {
  const int lineH12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineH10 = renderer.getLineHeight(UI_10_FONT_ID);
  char buf[96];

  if (weatherLoading_) {
    renderer.drawText(UI_12_FONT_ID, x, y, tr(STR_LOADING), true);
    return;
  }
  if (!weather_.valid) {
    if (weatherError_ != Weather::Client::Result::Ok) {
      renderer.drawText(UI_10_FONT_ID, x, y, weatherErrorLabel(weatherError_), true);
    } else {
      renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_REFRESH_WEATHER), true);
    }
    return;
  }

  const char* city = weather_.city[0] ? weather_.city : tr(STR_WEATHER);
  snprintf(buf, sizeof(buf), "%s %d°C %s%s", city, static_cast<int>(weather_.currentTemp),
           weatherLabel(weather_.currentCategory), weather_.stale ? " (c)" : "");
  const auto current = renderer.truncatedText(UI_12_FONT_ID, buf, width, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, x, y, current.c_str(), true, EpdFontFamily::BOLD);
  y += lineH12 + 2;

  const uint8_t days = std::min<uint8_t>(weather_.count, FORECAST_DAYS);
  for (uint8_t i = 0; i < days; i++) {
    const Weather::Day& day = weather_.days[i];
    // date is YYYY-MM-DD; only MM-DD fits alongside the numbers at this width.
    const char* shortDate = strlen(day.date) >= 10 ? day.date + 5 : day.date;
    snprintf(buf, sizeof(buf), "%s %s %d°/%d° %u%%", shortDate, weatherLabel(day.category), static_cast<int>(day.hi),
             static_cast<int>(day.lo), static_cast<unsigned>(day.rain));
    renderer.drawText(UI_10_FONT_ID, x, y, buf, true);
    y += lineH10;
  }
}