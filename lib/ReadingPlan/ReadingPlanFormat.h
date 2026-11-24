#pragma once

#include <cstddef>
#include <cstdint>

// RP-01/02 (v4.0 plan §4.4/§4.5): on-disk format for reading plans and free
// time slots. Deliberately independent of the SD layer so the encode/decode
// rules can be covered by host tests; ReadingPlanStore.cpp owns the I/O.
//
// Both files are compact, one record per line, with a stable field order (the
// same shape as the RH history files). Records are small: 8 plans x ~250 B and
// 8 slots x ~80 B, so a single 4 KB read buffer holds a whole file.
namespace ReadingPlan {

inline constexpr const char* DIR = "/.crosspoint/plans";
inline constexpr const char* PLANS_PATH = "/.crosspoint/plans/plans.json";
inline constexpr const char* SLOTS_PATH = "/.crosspoint/plans/slots.json";

inline constexpr int MAX_PLANS = 8;
inline constexpr int MAX_SLOTS = 8;

inline constexpr int ID_CAP = 24;
inline constexpr int BOOK_CAP = 96;
inline constexpr int ISO_CAP = 32;  // "YYYY-MM-DDTHH:MM:SS+HH:MM" is 25 chars
inline constexpr int DATE_CAP = 11;
inline constexpr int NAME_CAP = 40;

// v4.0 §4.4 status values. Archived plans stay in the file (归档不丢数据) and
// remain editable; the list only changes their displayed state.
enum class PlanStatus : uint8_t { Active = 0, Paused = 1, Archived = 2 };

struct Plan {
  char id[ID_CAP] = {};
  char bookId[BOOK_CAP] = {};
  char createdAt[ISO_CAP] = {};    // ISO-8601 local time, set on first save
  char targetDate[DATE_CAP] = {};  // YYYY-MM-DD, empty = no deadline
  char pausedUntil[DATE_CAP] = {};
  uint16_t weeklyMinutes = 180;
  uint16_t weeklyPages = 0;  // 0 = pages not tracked
  uint8_t priority = 3;      // 1 (highest) .. 5
  int8_t tzOffsetQ = 48;     // biased quarter-hours, same encoding as settings
  PlanStatus status = PlanStatus::Active;
};

struct Plans {
  Plan items[MAX_PLANS];
  int count = 0;
};

struct FreeSlot {
  char name[NAME_CAP] = {};
  uint8_t daysMask = 0;  // bit 0 = Sunday .. bit 6 = Saturday
  uint16_t startMin = 0;
  uint16_t endMin = 0;
  bool flexible = false;
};

struct Slots {
  FreeSlot items[MAX_SLOTS];
  int count = 0;
};

bool isValidDate(const char* value);  // "YYYY-MM-DD", real calendar date
bool isValidTime(const char* value);  // "HH:MM"
bool parseHhMm(const char* value, uint16_t& minutesOut);
void formatHhMm(char* out, size_t cap, uint16_t minutes);

uint8_t dayBit(int weekday);  // 0 = Sunday .. 6 = Saturday; 0 when invalid
bool daySelected(uint8_t mask, int weekday);
int dayCount(uint8_t mask);

const char* statusKey(PlanStatus status);
bool parseStatus(const char* value, size_t len, PlanStatus& out);

// "plan_YYYYMMDD_NNN". `date` may be empty or invalid, in which case the date
// part is zeroed (the id still has to be unique and stable on the card).
void makePlanId(char* out, size_t cap, const char* date, int sequence);

// One plan object, no trailing newline. Returns bytes written (excl. NUL) or 0.
size_t formatPlanLine(char* out, size_t cap, const Plan& plan);
// Whole file. Returns 0 when the buffer is too small.
size_t formatPlansFile(char* out, size_t cap, const Plans& plans);
// Number of plans accepted (invalid lines and overflow are skipped).
int parsePlansFile(const char* text, size_t len, Plans& plans);

size_t formatSlotsFile(char* out, size_t cap, const Slots& slots, const char* updatedAtIso);
int parseSlotsFile(const char* text, size_t len, Slots& slots);

}  // namespace ReadingPlan
