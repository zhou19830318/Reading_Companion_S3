#include "Almanac.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace Almanac {
namespace {

using i64 = int64_t;

// Lunar month table, 1900..2100, one uint32 per year:
//   bits 16..4 : 1 = the corresponding lunar month has 30 days, 0 = 29
//   bits 3..0  : leap month number, 0 = no leap month
//   bit 20     : leap month has 30 days (when a leap month exists)
// Source: Hong Kong Observatory conversion tables, as published in
// vsme/chinese-days (src/solar_lunar/constants.ts). Verified against known
// dates in test/almanac/AlmanacTest.cpp.
constexpr uint32_t LUNAR_INFO[201] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2,  // 1900-1909
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977,  // 1910-1919
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970,  // 1920-1929
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950,  // 1930-1939
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557,  // 1940-1949
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0,  // 1950-1959
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0,  // 1960-1969
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6,  // 1970-1979
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570,  // 1980-1989
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x055c0, 0x0ab60, 0x096d5, 0x092e0,  // 1990-1999
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,  // 2000-2009
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,  // 2010-2019
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,  // 2020-2029
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,  // 2030-2039
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,  // 2040-2049
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0,  // 2050-2059
    0x0a2e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4,  // 2060-2069
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0,  // 2070-2079
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160,  // 2080-2089
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a2d0, 0x0d150, 0x0f252,  // 2090-2099
    0x0d520,                                                                                   // 2100
};

// Solar longitude (degrees) each term sits at. Index 0 = 小寒 (285°), rising by
// 15° per term; 春分 (index 5) is 0°.
constexpr int16_t TERM_LONGITUDE[TERM_COUNT] = {285, 300, 315, 330, 345, 0,   15,  30,  45,  60,  75,  90,
                                                105, 120, 135, 150, 165, 180, 195, 210, 225, 240, 255, 270};

constexpr const char* TERM_NAMES[TERM_COUNT] = {
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨", "立夏", "小满", "芒种", "夏至",
    "小暑", "大暑", "立秋", "处暑", "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
};

constexpr const char* STEMS[10] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
constexpr const char* BRANCHES[12] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};

constexpr const char* CYCLE[60] = {
    "甲子", "乙丑", "丙寅", "丁卯", "戊辰", "己巳", "庚午", "辛未", "壬申", "癸酉",  //
    "甲戌", "乙亥", "丙子", "丁丑", "戊寅", "己卯", "庚辰", "辛巳", "壬午", "癸未",  //
    "甲申", "乙酉", "丙戌", "丁亥", "戊子", "己丑", "庚寅", "辛卯", "壬辰", "癸巳",  //
    "甲午", "乙未", "丙申", "丁酉", "戊戌", "己亥", "庚子", "辛丑", "壬寅", "癸卯",  //
    "甲辰", "乙巳", "丙午", "丁未", "戊申", "己酉", "庚戌", "辛亥", "壬子", "癸丑",  //
    "甲寅", "乙卯", "丙辰", "丁巳", "戊午", "己未", "庚申", "辛酉", "壬戌", "癸亥",
};

constexpr const char* ZODIACS[12] = {"鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪"};

constexpr const char* MONTH_NAMES[12] = {"正月", "二月", "三月", "四月", "五月", "六月",
                                         "七月", "八月", "九月", "十月", "冬月", "腊月"};

constexpr const char* DAY_NAMES[30] = {"初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
                                       "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
                                       "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};

// 建除十二神, in the order the officer progresses from the 月建.
constexpr const char* OFFICERS[12] = {"建", "除", "满", "平", "定", "执", "破", "危", "成", "收", "开", "闭"};
constexpr const char* OFFICER_YI[12] = {
    "出行 上任 见贵", "除服 疗病 扫舍", "祈福 嫁娶 开市", "修造 涂泥 平治", "嫁娶 入宅 开市", "祭祀 祈福 捕捉",
    "破屋 坏垣 求医", "祭祀 安床 立券", "开市 嫁娶 入宅", "纳财 捕捉 入仓", "祈福 开市 出行", "筑堤 补垣 安葬",
};
constexpr const char* OFFICER_JI[12] = {
    "动土 开仓 掘井", "出行 嫁娶 开市", "服药 安葬 赴任", "开渠 栽种 远行", "诉讼 出行 移徙", "开市 迁移 落成",
    "嫁娶 开市 上任", "登高 行船 远行", "诉讼 动土 破土", "开市 出行 安葬", "安葬 动土 伐木", "开市 出行 求医",
};

// Days since 1970-01-01 (Howard Hinnant's civil calendar algorithms). Proleptic
// Gregorian, valid for any year we can feed it.
constexpr i64 daysFromCivil(int y, unsigned m, unsigned d) {
  y -= static_cast<int>(m <= 2);
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + static_cast<i64>(doe) - 719468;
}

constexpr void civilFromDays(i64 z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const i64 era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(yoe + era * 400) + static_cast<int>(m <= 2);
}

constexpr uint32_t leapMonthOf(uint16_t year) { return LUNAR_INFO[year - 1900] & 0x0Fu; }

constexpr uint32_t leapDaysOf(uint16_t year) {
  if (leapMonthOf(year) == 0) return 0;
  return (LUNAR_INFO[year - 1900] & 0x10000u) ? 30u : 29u;
}

constexpr uint32_t monthDaysOf(uint16_t year, uint8_t month) {
  return (LUNAR_INFO[year - 1900] & (0x10000u >> month)) ? 30u : 29u;
}

uint32_t lunarYearDays(uint16_t year) {
  uint32_t total = 0;
  for (uint8_t m = 1; m <= 12; m++) total += monthDaysOf(year, m);
  return total + leapDaysOf(year);
}

// 月建地支 index for the 节气 month containing the date (小寒 = 丑, 立春 = 寅, ...).
uint8_t monthBranchOf(uint16_t year, uint8_t month, uint8_t day) {
  int term = 2 * (static_cast<int>(month) - 1);  // the 节 that opens this civil month
  if (static_cast<int>(day) < solarTermDay(year, static_cast<uint8_t>(term))) term -= 2;
  if (term < 0) return 0;  // before 小寒: still the previous year's 子月
  return static_cast<uint8_t>((term / 2 + 1) % 12);
}

uint32_t dayCycleIndex(uint16_t year, uint8_t month, uint8_t day) {
  // (JDN + 49) % 60 == 0 is 甲子. Anchors cross-checked against published
  // almanacs: 2026-07-01 = 丙子 (index 12) and 2026-11-18 = 丙申 (index 32).
  const i64 jdn = daysFromCivil(year, month, day) + 2440588;
  const int64_t index = (jdn + 49) % 60;
  return static_cast<uint32_t>(index < 0 ? index + 60 : index);
}

uint16_t ganZhiYearFor(uint16_t year, uint8_t month, uint8_t day) {
  // The 干支 year turns at 立春, not at the lunar new year.
  if (month <= 2) {
    const int lichun = solarTermDay(year, 2);
    if (month == 1 || static_cast<int>(day) < lichun) return static_cast<uint16_t>(year - 1);
  }
  return year;
}

}  // namespace

bool isSupported(uint16_t year, uint8_t month, uint8_t day) {
  if (year < 1900 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) return false;
  return daysFromCivil(year, month, day) >= daysFromCivil(1900, 1, 31);
}

bool solarToLunar(uint16_t year, uint8_t month, uint8_t day, LunarDate& out) {
  out = LunarDate{};
  if (!isSupported(year, month, day)) return false;

  i64 remaining = daysFromCivil(year, month, day) - daysFromCivil(1900, 1, 31);
  uint16_t lunarYear = 1900;
  while (lunarYear <= 2100) {
    const uint32_t days = lunarYearDays(lunarYear);
    if (remaining < static_cast<i64>(days)) break;
    remaining -= days;
    lunarYear++;
  }
  if (lunarYear > 2100) return false;

  const uint32_t leapMonth = leapMonthOf(lunarYear);
  uint8_t lunarMonth = 1;
  bool leap = false;
  while (true) {
    const uint32_t days = leap ? leapDaysOf(lunarYear) : monthDaysOf(lunarYear, lunarMonth);
    if (remaining < static_cast<i64>(days)) break;
    remaining -= days;
    if (!leap && leapMonth == lunarMonth) {
      leap = true;
    } else {
      leap = false;
      if (++lunarMonth > 12) return false;  // table ran out: cannot happen inside range
    }
  }

  out.year = lunarYear;
  out.month = lunarMonth;
  out.day = static_cast<uint8_t>(remaining + 1);
  out.leap = leap;
  return true;
}

const char* monthName(uint8_t month, bool leap) {
  static const char* const LEAP_MONTH_NAMES[12] = {"闰正月", "闰二月", "闰三月", "闰四月", "闰五月", "闰六月",
                                                   "闰七月", "闰八月", "闰九月", "闰十月", "闰冬月", "闰腊月"};
  if (month < 1 || month > 12) return "";
  if (leap) return LEAP_MONTH_NAMES[month - 1];
  return MONTH_NAMES[month - 1];
}

const char* dayName(uint8_t day) {
  if (day < 1 || day > 30) return "";
  return DAY_NAMES[day - 1];
}

const char* yearGanZhi(uint16_t lunarYear) {
  const uint32_t index = (static_cast<uint32_t>(lunarYear) + 60 - 4) % 60;
  return CYCLE[index];
}

const char* zodiac(uint16_t lunarYear) {
  const uint32_t index = (static_cast<uint32_t>(lunarYear) + 12 - 4) % 12;
  return ZODIACS[index];
}

void dayGanZhi(uint16_t year, uint8_t month, uint8_t day, char* buf, size_t bufSize) {
  if (buf == nullptr || bufSize < 7) return;
  snprintf(buf, bufSize, "%s", CYCLE[dayCycleIndex(year, month, day)]);
}

void monthGanZhi(uint16_t year, uint8_t month, uint8_t day, char* buf, size_t bufSize) {
  if (buf == nullptr || bufSize < 7) return;
  const uint32_t gzYear = ganZhiYearFor(year, month, day);
  const uint32_t yearStem = (gzYear + 60 - 4) % 10;
  const uint8_t branch = monthBranchOf(year, month, day);
  // 寅月's stem follows the year stem (甲己之年丙作首 ...), then步进 to this branch.
  const uint32_t yinStem = (yearStem % 5) * 2 + 2;
  const uint32_t branchFromYin = (branch + 12 - 2) % 12;
  const uint32_t stem = (yinStem + branchFromYin) % 10;
  snprintf(buf, bufSize, "%s%s", STEMS[stem], BRANCHES[branch]);
}

namespace {

// Apparent geocentric solar longitude (degrees), low-precision series from the
// Astronomical Almanac. Accurate to ~0.01° (≈15 min of time), which is far below
// the day granularity the calendar needs.
double apparentSolarLongitude(double jd) {
  const double t = (jd - 2451545.0) / 36525.0;
  const double l0 = 280.46646 + 36000.76983 * t + 0.0003032 * t * t;
  const double m = 357.52911 + 35999.05029 * t - 0.0001537 * t * t;
  const double c = (1.914602 - 0.004817 * t - 0.000014 * t * t) * std::sin(m * M_PI / 180.0) +
                   (0.019993 - 0.000101 * t) * std::sin(2.0 * m * M_PI / 180.0) +
                   0.000289 * std::sin(3.0 * m * M_PI / 180.0);
  const double omega = 125.04 - 1934.136 * t;
  double longitude = l0 + c - 0.00569 - 0.00478 * std::sin(omega * M_PI / 180.0);
  longitude = std::fmod(longitude, 360.0);
  if (longitude < 0.0) longitude += 360.0;
  return longitude;
}

// Instant (Julian Day, UT) at which the sun reaches the term's longitude, found
// by bisection on a monotonic function.
double solarTermInstant(uint16_t year, uint8_t termIndex) {
  const double start = static_cast<double>(daysFromCivil(year, 1, 1) + 2440588);
  const double guessLongitude = static_cast<double>(TERM_LONGITUDE[termIndex]);
  // 小寒 is ~5 January, then one term per 15.2184 days. The ±10 day bracket
  // keeps the longitude difference monotonic (the sun moves ~10° over it), so
  // the plain bisection below converges on the right crossing.
  const double estimate = start + 4.0 + 15.2184 * static_cast<double>(termIndex);
  double lo = estimate - 10.0;
  double hi = estimate + 10.0;
  for (int i = 0; i < 40; i++) {
    const double mid = 0.5 * (lo + hi);
    double diff = apparentSolarLongitude(mid) - guessLongitude;
    while (diff > 180.0) diff -= 360.0;
    while (diff < -180.0) diff += 360.0;
    if (diff < 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

}  // namespace

int solarTermDay(uint16_t year, uint8_t termIndex) {
  if (year < 1900 || year > 2100 || termIndex >= TERM_COUNT) return -1;
  const double jd = solarTermInstant(year, termIndex);
  // Terms are published in Beijing time (UTC+8): shift the instant into local
  // time before taking the civil date.
  const i64 jdnLocal = static_cast<i64>(std::floor(jd + 8.0 / 24.0 + 0.5));
  int y = 0;
  unsigned m = 0;
  unsigned d = 0;
  civilFromDays(jdnLocal - 2440588, y, m, d);
  return static_cast<int>(d);
}

const char* solarTermName(uint8_t termIndex) {
  if (termIndex >= TERM_COUNT) return "";
  return TERM_NAMES[termIndex];
}

int solarTermOn(uint16_t year, uint8_t month, uint8_t day) {
  if (month < 1 || month > 12) return -1;
  for (uint8_t i = 0; i < 2; i++) {
    const uint8_t index = static_cast<uint8_t>(2 * (month - 1) + i);
    if (solarTermDay(year, index) == static_cast<int>(day)) return index;
  }
  return -1;
}

int nextSolarTerm(uint16_t year, uint8_t month, uint8_t day, uint16_t& termYear, uint8_t& termMonth, uint8_t& termDay) {
  if (month < 1 || month > 12) return -1;
  // Walk forward from this month's first term; at most two years ahead, since
  // the next term is always within ~16 days.
  for (uint16_t y = year; y <= year + 1 && y <= 2100; y++) {
    const uint8_t firstMonth = (y == year) ? month : 1;
    for (uint8_t m = firstMonth; m <= 12; m++) {
      for (uint8_t i = 0; i < 2; i++) {
        const uint8_t index = static_cast<uint8_t>(2 * (m - 1) + i);
        const int termDayOfMonth = solarTermDay(y, index);
        if (termDayOfMonth < 0) continue;
        const i64 delta = daysFromCivil(y, m, static_cast<uint8_t>(termDayOfMonth)) - daysFromCivil(year, month, day);
        if (delta > 0) {
          termYear = y;
          termMonth = m;
          termDay = static_cast<uint8_t>(termDayOfMonth);
          return index;
        }
      }
    }
  }
  return -1;
}

const char* jianChu(uint16_t year, uint8_t month, uint8_t day) {
  if (month < 1 || month > 12 || day < 1 || day > 31) return "";
  const uint8_t monthBranch = monthBranchOf(year, month, day);
  const uint8_t dayBranch = static_cast<uint8_t>(dayCycleIndex(year, month, day) % 12);
  return OFFICERS[(dayBranch + 12 - monthBranch) % 12];
}

const char* yi(uint16_t year, uint8_t month, uint8_t day) {
  if (month < 1 || month > 12 || day < 1 || day > 31) return "";
  const uint8_t monthBranch = monthBranchOf(year, month, day);
  const uint8_t dayBranch = static_cast<uint8_t>(dayCycleIndex(year, month, day) % 12);
  return OFFICER_YI[(dayBranch + 12 - monthBranch) % 12];
}

const char* ji(uint16_t year, uint8_t month, uint8_t day) {
  if (month < 1 || month > 12 || day < 1 || day > 31) return "";
  const uint8_t monthBranch = monthBranchOf(year, month, day);
  const uint8_t dayBranch = static_cast<uint8_t>(dayCycleIndex(year, month, day) % 12);
  return OFFICER_JI[(dayBranch + 12 - monthBranch) % 12];
}

namespace {

// 六十甲子纳音, one entry per stem-branch pair (cycle index / 2).
constexpr const char* NA_YIN[30] = {
    "海中金", "炉中火", "大林木", "路旁土", "剑锋金", "山头火", "涧下水", "城头土", "白蜡金", "杨柳木",
    "泉中水", "屋上土", "霹雳火", "松柏木", "长流水", "沙中金", "山下火", "平地木", "壁上土", "金箔金",
    "覆灯火", "天河水", "大驿土", "钗钏金", "桑柘木", "大溪水", "沙中土", "天上火", "石榴木", "大海水",
};

// 财神方位 by day stem pair (甲乙 / 丙丁 / 戊己 / 庚辛 / 壬癸).
constexpr const char* WEALTH_DIR[5] = {"东北", "西南", "正北", "正东", "正南"};
// 喜神方位 by day stem (甲己 / 乙庚 / 丙辛 / 丁壬 / 戊癸).
constexpr const char* JOY_DIR[10] = {"东北", "西北", "西南", "正南", "东南", "东北", "西北", "西南", "正南", "东南"};

// 煞 direction by the day branch's 三合 group: 申子辰煞南, 巳酉丑煞东,
// 寅午戌煞北, 亥卯未煞西. The group is exactly branch % 4.
constexpr const char* SHA_DIR[4] = {"南", "东", "北", "西"};

}  // namespace

void dayChongSha(uint16_t year, uint8_t month, uint8_t day, char* buf, size_t bufSize) {
  if (buf == nullptr || bufSize == 0) return;
  buf[0] = '\0';
  if (month < 1 || month > 12 || day < 1 || day > 31 || bufSize < 16) return;
  const uint8_t dayBranch = static_cast<uint8_t>(dayCycleIndex(year, month, day) % 12);
  const uint8_t clash = static_cast<uint8_t>((dayBranch + 6) % 12);
  snprintf(buf, bufSize, "冲%s煞%s", ZODIACS[clash], SHA_DIR[dayBranch % 4]);
}

const char* dayNaYin(uint16_t year, uint8_t month, uint8_t day) {
  if (month < 1 || month > 12 || day < 1 || day > 31) return "";
  return NA_YIN[dayCycleIndex(year, month, day) / 2];
}

const char* dayWealthDirection(uint16_t year, uint8_t month, uint8_t day) {
  if (month < 1 || month > 12 || day < 1 || day > 31) return "";
  return WEALTH_DIR[(dayCycleIndex(year, month, day) % 10) / 2];
}

const char* dayJoyDirection(uint16_t year, uint8_t month, uint8_t day) {
  if (month < 1 || month > 12 || day < 1 || day > 31) return "";
  return JOY_DIR[dayCycleIndex(year, month, day) % 10];
}

int prevSolarTerm(uint16_t year, uint8_t month, uint8_t day, uint16_t& termYear, uint8_t& termMonth, uint8_t& termDay) {
  if (month < 1 || month > 12) return -1;
  // Walk backward from this month's last term; at most two years back, since the
  // previous term is always within ~16 days.
  for (int y = static_cast<int>(year); y >= static_cast<int>(year) - 1 && y >= 1900; y--) {
    const uint8_t lastMonth = (y == static_cast<int>(year)) ? month : 12;
    for (int m = lastMonth; m >= 1; m--) {
      for (int i = 1; i >= 0; i--) {
        const uint8_t index = static_cast<uint8_t>(2 * (m - 1) + i);
        const int termDayOfMonth = solarTermDay(static_cast<uint16_t>(y), index);
        if (termDayOfMonth < 0) continue;
        const i64 delta = daysFromCivil(static_cast<uint16_t>(y), static_cast<uint8_t>(m),
                                        static_cast<uint8_t>(termDayOfMonth)) -
                          daysFromCivil(year, month, day);
        if (delta < 0) {
          termYear = static_cast<uint16_t>(y);
          termMonth = static_cast<uint8_t>(m);
          termDay = static_cast<uint8_t>(termDayOfMonth);
          return index;
        }
      }
    }
  }
  return -1;
}

}  // namespace Almanac
