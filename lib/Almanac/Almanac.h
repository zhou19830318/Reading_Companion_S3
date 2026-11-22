#pragma once

#include <cstddef>
#include <cstdint>

// Chinese calendar (农历) helpers for the 万年历 screen.
//
// Scope, stated honestly: this is the civil calendar plus the *textbook*
// almanac layer — lunar date, 24 solar terms, 干支, 生肖, the 建除十二神
// with their conventional 宜/忌 lists, and the rule-derived day fields
// (冲煞, 纳音五行, 财神/喜神方位). It is not a 通书: the full 神煞 almanac
// (吉神宜趋/凶煞宜忌, 二十八宿, 六曜, 彭祖百忌) is deliberately absent, and no
// UI text claims otherwise.
//
// All functions are pure, allocation-free and host-testable (test/almanac).
namespace Almanac {

struct LunarDate {
  uint16_t year = 0;  // lunar year
  uint8_t month = 0;  // 1..12
  uint8_t day = 0;    // 1..30
  bool leap = false;
};

// Supported range: 1900-01-31 .. 2100-12-31 (the span of the lunar table).
bool isSupported(uint16_t year, uint8_t month, uint8_t day);

// Gregorian -> lunar. Returns false outside the table's range.
bool solarToLunar(uint16_t year, uint8_t month, uint8_t day, LunarDate& out);

// Chinese names (UTF-8, static storage in flash). Month name carries the 闰
// prefix itself when leap is set.
const char* monthName(uint8_t month, bool leap);
const char* dayName(uint8_t day);

// 丙午 for 2026. Uses the lunar year, so a date in January still belongs to the
// previous 干支 year.
const char* yearGanZhi(uint16_t lunarYear);
// 马 for 2026.
const char* zodiac(uint16_t lunarYear);
// 日柱, e.g. 丙申. buf must hold at least 7 bytes (two 3-byte characters + NUL).
// The pillar follows the civil date, not a 23:00 boundary.
void dayGanZhi(uint16_t year, uint8_t month, uint8_t day, char* buf, size_t bufSize);
// 月柱, e.g. 己亥. Same buffer requirement.
void monthGanZhi(uint16_t year, uint8_t month, uint8_t day, char* buf, size_t bufSize);

// --- 24 solar terms ---
// Index 0 = 小寒 (January), 1 = 大寒, 2 = 立春 ... 23 = 冬至 (December).
constexpr uint8_t TERM_COUNT = 24;
int solarTermDay(uint16_t year, uint8_t termIndex);
const char* solarTermName(uint8_t termIndex);
// Term falling on the given date, or -1.
int solarTermOn(uint16_t year, uint8_t month, uint8_t day);
// Next term strictly after the given date. Fills the term's civil date and
// returns its index; returns -1 past 2100.
int nextSolarTerm(uint16_t year, uint8_t month, uint8_t day, uint16_t& termYear, uint8_t& termMonth, uint8_t& termDay);

// --- 建除十二神 ---
// Textbook rule: the officer is the day branch counted from the month branch
// (月建), which itself follows the 节 boundaries (立春 = 寅月, 惊蛰 = 卯月, ...).
// Commercial 黄历 sites disagree with each other here, so this follows the rule
// rather than copying any one of them.
const char* jianChu(uint16_t year, uint8_t month, uint8_t day);
// Conventional short 宜/忌 lists for that officer, space-separated.
const char* yi(uint16_t year, uint8_t month, uint8_t day);
const char* ji(uint16_t year, uint8_t month, uint8_t day);

// --- old-almanac day fields (textbook rules, same spirit as 建除) ---
// 冲煞, e.g. 冲虎煞南: the day branch clashes with the opposite branch's zodiac
// and the 煞 direction follows the day branch's 三合 group. buf >= 16 bytes.
void dayChongSha(uint16_t year, uint8_t month, uint8_t day, char* buf, size_t bufSize);
// 日柱纳音五行, e.g. 山下火 for 丙申. Static flash storage.
const char* dayNaYin(uint16_t year, uint8_t month, uint8_t day);
// 财神 / 喜神方位 by day stem, e.g. 西南. Static flash storage.
const char* dayWealthDirection(uint16_t year, uint8_t month, uint8_t day);
const char* dayJoyDirection(uint16_t year, uint8_t month, uint8_t day);
// Previous term strictly before the given date; fills its civil date and returns
// the index, or -1 before 1900.
int prevSolarTerm(uint16_t year, uint8_t month, uint8_t day, uint16_t& termYear, uint8_t& termMonth, uint8_t& termDay);

}  // namespace Almanac
