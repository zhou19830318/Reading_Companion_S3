#include "ReadingPlanFormat.h"

#include <cstdio>
#include <cstring>

namespace ReadingPlan {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

int twoDigits(const char* p) { return (p[0] - '0') * 10 + (p[1] - '0'); }

int fourDigits(const char* p) { return (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0'); }

bool isLeapYear(int year) { return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0); }

int daysInMonth(int year, int month) {
  static constexpr uint8_t DAYS[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && isLeapYear(year)) return 29;
  return DAYS[month];
}

// ---- bounded scanning helpers (the file buffer is not NUL-terminated) ----

const char* findToken(const char* begin, const char* end, const char* token, size_t tokenLen) {
  if (tokenLen == 0 || begin >= end) return nullptr;
  for (const char* p = begin; p + tokenLen <= end; ++p) {
    if (memcmp(p, token, tokenLen) == 0) return p;
  }
  return nullptr;
}

const char* findToken(const char* begin, const char* end, const char* token) {
  return findToken(begin, end, token, strlen(token));
}

bool hex4(const char* p, const char* end, uint32_t& value) {
  if (p + 4 > end) return false;
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9')
      v |= static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      v |= static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v |= static_cast<uint32_t>(c - 'A' + 10);
    else
      return false;
  }
  value = v;
  return true;
}

// Decode a JSON string value starting at the opening quote. Supports the
// escapes our writer can produce; \u is encoded back to UTF-8 (BMP only).
bool decodeString(const char*& p, const char* end, char* out, size_t cap) {
  if (p >= end || *p != '"' || cap == 0) return false;
  ++p;
  size_t w = 0;
  while (p < end && *p != '"') {
    unsigned char c = static_cast<unsigned char>(*p++);
    if (c == '\\') {
      if (p >= end) return false;
      const char esc = *p++;
      switch (esc) {
        case '"':
        case '\\':
        case '/':
          c = static_cast<unsigned char>(esc);
          break;
        case 'b':
          c = '\b';
          break;
        case 'f':
          c = '\f';
          break;
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(p, end, cp)) return false;
          p += 4;
          char enc[4];
          int n = 0;
          if (cp < 0x80) {
            enc[n++] = static_cast<char>(cp);
          } else if (cp < 0x800) {
            enc[n++] = static_cast<char>(0xC0 | (cp >> 6));
            enc[n++] = static_cast<char>(0x80 | (cp & 0x3F));
          } else {
            enc[n++] = static_cast<char>(0xE0 | (cp >> 12));
            enc[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            enc[n++] = static_cast<char>(0x80 | (cp & 0x3F));
          }
          if (w + static_cast<size_t>(n) + 1 > cap) return false;
          memcpy(out + w, enc, static_cast<size_t>(n));
          w += static_cast<size_t>(n);
          continue;
        }
        default:
          return false;
      }
    }
    if (w + 1 >= cap) return false;
    out[w++] = static_cast<char>(c);
  }
  if (p >= end || *p != '"') return false;  // unterminated
  ++p;
  out[w] = '\0';
  return true;
}

bool stringField(const char* begin, const char* end, const char* key, char* out, size_t cap) {
  const char* p = findToken(begin, end, key);
  if (p == nullptr) return false;
  p += strlen(key);
  while (p < end && (*p == ' ' || *p == '\t')) ++p;
  return decodeString(p, end, out, cap);
}

bool uintField(const char* begin, const char* end, const char* key, uint32_t& out) {
  const char* p = findToken(begin, end, key);
  if (p == nullptr) return false;
  p += strlen(key);
  while (p < end && (*p == ' ' || *p == '\t')) ++p;
  if (p >= end || !isDigit(*p)) return false;
  uint32_t v = 0;
  while (p < end && isDigit(*p)) {
    v = v * 10 + static_cast<uint32_t>(*p - '0');
    if (v > 1000000) return false;
    ++p;
  }
  out = v;
  return true;
}

bool intField(const char* begin, const char* end, const char* key, int32_t& out) {
  const char* p = findToken(begin, end, key);
  if (p == nullptr) return false;
  p += strlen(key);
  while (p < end && (*p == ' ' || *p == '\t')) ++p;
  bool negative = false;
  if (p < end && *p == '-') {
    negative = true;
    ++p;
  }
  if (p >= end || !isDigit(*p)) return false;
  int32_t v = 0;
  while (p < end && isDigit(*p)) {
    v = v * 10 + (*p - '0');
    if (v > 1000000) return false;
    ++p;
  }
  out = negative ? -v : v;
  return true;
}

bool boolField(const char* begin, const char* end, const char* key) { return findToken(begin, end, key) != nullptr; }

bool parseDaysMask(const char* begin, const char* end, uint8_t& mask) {
  const char* p = findToken(begin, end, "\"days\":[");
  if (p == nullptr) return false;
  p += 8;
  uint8_t m = 0;
  while (p < end && *p != ']') {
    if (isDigit(*p)) {
      const int day = *p - '0';
      if (day > 6) return false;
      m |= dayBit(day);
    } else if (*p != ',' && *p != ' ' && *p != '\t') {
      return false;
    }
    ++p;
  }
  if (p >= end) return false;
  mask = m;
  return true;
}

size_t escapeTo(char* out, size_t cap, const char* in) {
  if (out == nullptr || cap == 0) return 0;
  size_t w = 0;
  for (const char* p = in; p != nullptr && *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const char* esc = nullptr;
    char buf[7] = {};
    if (c == '"')
      esc = "\\\"";
    else if (c == '\\')
      esc = "\\\\";
    else if (c == '\n')
      esc = "\\n";
    else if (c == '\r')
      esc = "\\r";
    else if (c == '\t')
      esc = "\\t";
    else if (c < 0x20) {
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      esc = buf;
    }
    const size_t need = esc != nullptr ? strlen(esc) : 1;
    if (w + need + 1 > cap) return 0;
    if (esc != nullptr) {
      memcpy(out + w, esc, need);
    } else {
      out[w] = static_cast<char>(c);
    }
    w += need;
  }
  out[w] = '\0';
  return w;
}

bool parsePlanRecord(const char* begin, const char* end, Plan& plan) {
  plan = Plan{};

  if (!stringField(begin, end, "\"id\":", plan.id, sizeof(plan.id)) || plan.id[0] == '\0') return false;
  stringField(begin, end, "\"book_id\":", plan.bookId, sizeof(plan.bookId));
  stringField(begin, end, "\"created_at\":", plan.createdAt, sizeof(plan.createdAt));

  char status[16] = {};
  if (stringField(begin, end, "\"status\":", status, sizeof(status))) {
    PlanStatus parsed = PlanStatus::Active;
    if (parseStatus(status, strlen(status), parsed)) plan.status = parsed;
  }

  if (stringField(begin, end, "\"target_date\":", plan.targetDate, sizeof(plan.targetDate)) &&
      plan.targetDate[0] != '\0' && !isValidDate(plan.targetDate)) {
    return false;
  }
  if (stringField(begin, end, "\"paused_until\":", plan.pausedUntil, sizeof(plan.pausedUntil)) &&
      plan.pausedUntil[0] != '\0' && !isValidDate(plan.pausedUntil)) {
    plan.pausedUntil[0] = '\0';
  }

  uint32_t minutes = 180;
  if (uintField(begin, end, "\"value\":", minutes)) {
    if (minutes < 1) minutes = 1;
    if (minutes > 10080) minutes = 10080;
    plan.weeklyMinutes = static_cast<uint16_t>(minutes);
  }
  uint32_t pages = 0;
  if (uintField(begin, end, "\"pages_per_week\":", pages)) {
    plan.weeklyPages = static_cast<uint16_t>(pages > 10000 ? 10000 : pages);
  }
  uint32_t priority = 3;
  if (uintField(begin, end, "\"priority\":", priority)) {
    if (priority < 1) priority = 1;
    if (priority > 5) priority = 5;
    plan.priority = static_cast<uint8_t>(priority);
  }
  int32_t tz = 48;
  if (intField(begin, end, "\"timezone_offset_q\":", tz)) {
    if (tz < -48) tz = -48;
    if (tz > 104) tz = 104;
    plan.tzOffsetQ = static_cast<int8_t>(tz);
  }
  return true;
}

bool parseSlotRecord(const char* begin, const char* end, FreeSlot& slot) {
  slot = FreeSlot{};
  if (!stringField(begin, end, "\"name\":", slot.name, sizeof(slot.name))) return false;

  uint8_t mask = 0;
  if (!parseDaysMask(begin, end, mask) || mask == 0) return false;

  char start[8] = {};
  char stop[8] = {};
  if (!stringField(begin, end, "\"start\":", start, sizeof(start))) return false;
  if (!stringField(begin, end, "\"end\":", stop, sizeof(stop))) return false;
  uint16_t startMin = 0;
  uint16_t endMin = 0;
  if (!parseHhMm(start, startMin) || !parseHhMm(stop, endMin)) return false;
  if (endMin <= startMin) return false;

  slot.daysMask = mask;
  slot.startMin = startMin;
  slot.endMin = endMin;
  slot.flexible = boolField(begin, end, "\"flexible\":true");
  return true;
}

template <typename Record, typename ParseFn>
int parseRecords(const char* text, size_t len, const char* prefix, Record* items, int maxItems, ParseFn parseOne) {
  if (text == nullptr || len == 0) return 0;
  int count = 0;
  const char* p = text;
  const char* end = text + len;
  while (p < end) {
    const char* lineEnd = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
    if (lineEnd == nullptr) lineEnd = end;

    const char* s = p;
    while (s < lineEnd && (*s == ' ' || *s == '\t' || *s == '\r')) ++s;
    const char* e = lineEnd;
    while (e > s && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) --e;

    const size_t prefixLen = strlen(prefix);
    if (static_cast<size_t>(e - s) >= prefixLen && memcmp(s, prefix, prefixLen) == 0) {
      Record record;
      if (parseOne(s, e, record)) {
        if (count < maxItems) items[count++] = record;
      }
    }
    p = lineEnd < end ? lineEnd + 1 : end;
  }
  return count;
}

}  // namespace

bool isValidDate(const char* value) {
  if (value == nullptr) return false;
  for (int i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) {
      if (value[i] != '-') return false;
    } else if (!isDigit(value[i])) {
      return false;
    }
  }
  if (value[10] != '\0') return false;
  const int year = fourDigits(value);
  const int month = twoDigits(value + 5);
  const int day = twoDigits(value + 8);
  if (year < 2000 || year > 2200) return false;
  if (month < 1 || month > 12) return false;
  return day >= 1 && day <= daysInMonth(year, month);
}

bool isValidTime(const char* value) {
  uint16_t ignored = 0;
  return parseHhMm(value, ignored);
}

bool parseHhMm(const char* value, uint16_t& minutesOut) {
  if (value == nullptr) return false;
  if (!isDigit(value[0]) || !isDigit(value[1]) || value[2] != ':' || !isDigit(value[3]) || !isDigit(value[4]) ||
      value[5] != '\0') {
    return false;
  }
  const int hour = twoDigits(value);
  const int minute = twoDigits(value + 3);
  if (hour > 23 || minute > 59) return false;
  minutesOut = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

void formatHhMm(char* out, size_t cap, uint16_t minutes) {
  if (out == nullptr || cap == 0) return;
  snprintf(out, cap, "%02u:%02u", static_cast<unsigned>(minutes / 60), static_cast<unsigned>(minutes % 60));
}

uint8_t dayBit(int weekday) {
  if (weekday < 0 || weekday > 6) return 0;
  return static_cast<uint8_t>(1u << weekday);
}

bool daySelected(uint8_t mask, int weekday) {
  const uint8_t bit = dayBit(weekday);
  return bit != 0 && (mask & bit) != 0;
}

int dayCount(uint8_t mask) {
  int n = 0;
  for (int i = 0; i < 7; ++i) {
    if (mask & dayBit(i)) ++n;
  }
  return n;
}

const char* statusKey(PlanStatus status) {
  switch (status) {
    case PlanStatus::Paused:
      return "paused";
    case PlanStatus::Archived:
      return "archived";
    case PlanStatus::Active:
    default:
      return "active";
  }
}

bool parseStatus(const char* value, size_t len, PlanStatus& out) {
  if (value == nullptr) return false;
  if (len == 6 && memcmp(value, "active", 6) == 0) {
    out = PlanStatus::Active;
    return true;
  }
  if (len == 6 && memcmp(value, "paused", 6) == 0) {
    out = PlanStatus::Paused;
    return true;
  }
  if (len == 8 && memcmp(value, "archived", 8) == 0) {
    out = PlanStatus::Archived;
    return true;
  }
  return false;
}

void makePlanId(char* out, size_t cap, const char* date, int sequence) {
  if (out == nullptr || cap == 0) return;
  int year = 0;
  int month = 0;
  int day = 0;
  if (date != nullptr && isValidDate(date)) {
    year = fourDigits(date);
    month = twoDigits(date + 5);
    day = twoDigits(date + 8);
  }
  int seq = sequence < 1 ? 1 : (sequence > 999 ? 999 : sequence);
  snprintf(out, cap, "plan_%04d%02d%02d_%03d", year, month, day, seq);
}

size_t formatPlanLine(char* out, size_t cap, const Plan& plan) {
  if (out == nullptr || cap == 0) return 0;
  char escapedId[64];
  char escapedBook[BOOK_CAP * 2 + 8];
  char escapedCreated[ISO_CAP * 2 + 8];
  char escapedPaused[DATE_CAP * 2 + 8];
  if (escapeTo(escapedId, sizeof(escapedId), plan.id) == 0 && plan.id[0] != '\0') return 0;
  if (escapeTo(escapedBook, sizeof(escapedBook), plan.bookId) == 0 && plan.bookId[0] != '\0') return 0;
  if (escapeTo(escapedCreated, sizeof(escapedCreated), plan.createdAt) == 0 && plan.createdAt[0] != '\0') return 0;
  if (escapeTo(escapedPaused, sizeof(escapedPaused), plan.pausedUntil) == 0 && plan.pausedUntil[0] != '\0') return 0;

  const int n =
      snprintf(out, cap,
               "{\"id\":\"%s\",\"book_id\":\"%s\",\"status\":\"%s\",\"created_at\":\"%s\",\"target_date\":\"%s\","
               "\"goal\":{\"type\":\"minutes_per_week\",\"value\":%u,\"pages_per_week\":%u},"
               "\"priority\":%u,\"timezone_offset_q\":%d,\"paused_until\":\"%s\"}",
               escapedId, escapedBook, statusKey(plan.status), escapedCreated, plan.targetDate,
               static_cast<unsigned>(plan.weeklyMinutes), static_cast<unsigned>(plan.weeklyPages),
               static_cast<unsigned>(plan.priority), static_cast<int>(plan.tzOffsetQ), escapedPaused);
  if (n <= 0 || static_cast<size_t>(n) >= cap) return 0;
  return static_cast<size_t>(n);
}

size_t formatPlansFile(char* out, size_t cap, const Plans& plans) {
  if (out == nullptr || cap == 0) return 0;
  if (plans.count <= 0) {
    const int n = snprintf(out, cap, "{\"schema_version\":1,\"plans\":[]}\n");
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
  }
  int n = snprintf(out, cap, "{\"schema_version\":1,\"plans\":[");
  if (n <= 0) return 0;
  size_t w = static_cast<size_t>(n);
  for (int i = 0; i < plans.count; ++i) {
    if (w + 2 >= cap) return 0;
    if (i > 0) out[w++] = ',';
    out[w++] = '\n';
    const size_t lineLen = formatPlanLine(out + w, cap - w, plans.items[i]);
    if (lineLen == 0) return 0;
    w += lineLen;
  }
  if (w + 4 >= cap) return 0;
  memcpy(out + w, "\n]}\n", 4);
  w += 4;
  out[w] = '\0';
  return w;
}

int parsePlansFile(const char* text, size_t len, Plans& plans) {
  plans.count = parseRecords(text, len, "{\"id\":", plans.items, MAX_PLANS, parsePlanRecord);
  return plans.count;
}

size_t formatSlotsFile(char* out, size_t cap, const Slots& slots, const char* updatedAtIso) {
  if (out == nullptr || cap == 0) return 0;
  char escapedUpdated[ISO_CAP * 2 + 8];
  if (escapeTo(escapedUpdated, sizeof(escapedUpdated), updatedAtIso != nullptr ? updatedAtIso : "") == 0 &&
      updatedAtIso != nullptr && updatedAtIso[0] != '\0') {
    return 0;
  }

  int n = snprintf(out, cap, "{\"schema_version\":1,\"updated_at\":\"%s\",\"slots\":[", escapedUpdated);
  if (n <= 0) return 0;
  size_t w = static_cast<size_t>(n);

  for (int i = 0; i < slots.count; ++i) {
    const FreeSlot& slot = slots.items[i];
    char escapedName[NAME_CAP * 2 + 8];
    if (escapeTo(escapedName, sizeof(escapedName), slot.name) == 0 && slot.name[0] != '\0') return 0;

    char days[24] = {};
    size_t d = 0;
    for (int day = 0; day < 7; ++day) {
      if (!daySelected(slot.daysMask, day)) continue;
      const int written = snprintf(days + d, sizeof(days) - d, "%s%d", d == 0 ? "" : ",", day);
      if (written <= 0) return 0;
      d += static_cast<size_t>(written);
    }

    char start[8];
    char stop[8];
    formatHhMm(start, sizeof(start), slot.startMin);
    formatHhMm(stop, sizeof(stop), slot.endMin);

    if (w + 2 >= cap) return 0;
    if (i > 0) out[w++] = ',';
    out[w++] = '\n';
    const int lineLen =
        snprintf(out + w, cap - w, "{\"name\":\"%s\",\"days\":[%s],\"start\":\"%s\",\"end\":\"%s\",\"flexible\":%s}",
                 escapedName, days, start, stop, slot.flexible ? "true" : "false");
    if (lineLen <= 0 || static_cast<size_t>(lineLen) >= cap - w) return 0;
    w += static_cast<size_t>(lineLen);
  }

  if (w + 4 >= cap) return 0;
  memcpy(out + w, "\n]}\n", 4);
  w += 4;
  out[w] = '\0';
  return w;
}

int parseSlotsFile(const char* text, size_t len, Slots& slots) {
  slots.count = parseRecords(text, len, "{\"name\":", slots.items, MAX_SLOTS, parseSlotRecord);
  return slots.count;
}

}  // namespace ReadingPlan
