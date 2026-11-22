#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Five-day forecast through the relay (`GET /api/v1/weather`).
//
// The relay does the geocoding from the requesting IP, so the device never
// picks a city and never talks to a third party (SCOPE.md keeps the relay as the
// only backend). The last successful answer is cached on the SD card so the
// calendar screen has something to show while offline.
namespace Weather {

constexpr uint8_t MAX_DAYS = 5;
constexpr uint8_t CATEGORY_COUNT = 8;

// Relay categories: 0 clear, 1 partly cloudy, 2 overcast, 3 fog, 4 drizzle,
// 5 rain, 6 snow, 7 thunder. Labels live in the UI layer (tr()).
struct Day {
  char date[11] = {};  // YYYY-MM-DD
  int8_t hi = 0;
  int8_t lo = 0;
  uint8_t rain = 0;  // percent
  uint8_t category = 0;
};

struct Report {
  bool valid = false;
  bool stale = false;  // relay served a cached forecast because upstream failed
  char city[48] = {};
  uint32_t stamp = 0;
  int8_t currentTemp = 0;
  uint8_t currentCategory = 0;
  uint8_t count = 0;
  Day days[MAX_DAYS] = {};
};

class Client {
 public:
  enum class Result : uint8_t { Ok, Config, Wifi, Auth, Unavailable, Memory, InvalidData };

  // Allocates the response buffer once; release with the owning activity.
  bool begin();

  Result fetch(Report& out);

  // SD cache at /.crosspoint/weather.bin. A version mismatch is treated as a
  // miss, so bumping the magic string is enough to invalidate old files.
  static bool loadCache(Report& out);
  static bool saveCache(const Report& report);

 private:
  static constexpr size_t BODY_CAP = 4096;
  std::unique_ptr<char[]> body;
};

}  // namespace Weather
