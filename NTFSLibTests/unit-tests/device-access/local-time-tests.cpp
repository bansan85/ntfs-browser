#include <ntfs-browser/win-types.h>

#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "local-time.h"

using NtfsDevice::LocalTime;
using NtfsDevice::UtcTicksToLocalTime;

namespace {

// 2026-01-01 00:00:00 UTC in 100 ns ticks since 1601-01-01.
constexpr ULONGLONG new_year_2026_ticks = 134'116'992'000'000'000ULL;
// Ticks in one second and in one minute.
constexpr ULONGLONG second_ticks = 10'000'000ULL;
constexpr ULONGLONG minute_ticks = 60ULL * second_ticks;

// Sets the process time zone to zone for its lifetime, then restores it. A
// POSIX TZ string such as "TST-2" (two hours east of UTC) needs no zone
// database on the machine.
class ScopedZone final {
 public:
  explicit ScopedZone(const char* zone) {
    const char* const previous = std::getenv("TZ");
    if (previous != nullptr) {
      previous_ = previous;
      had_previous_ = true;
    }
    setenv("TZ", zone, 1);
    tzset();
  }

  ScopedZone(const ScopedZone&) = delete;
  ScopedZone& operator=(const ScopedZone&) = delete;
  ScopedZone(ScopedZone&&) = delete;
  ScopedZone& operator=(ScopedZone&&) = delete;

  ~ScopedZone() {
    if (had_previous_) {
      setenv("TZ", previous_.c_str(), 1);
    } else {
      unsetenv("TZ");
    }
    tzset();
  }

 private:
  std::string previous_;
  bool had_previous_ = false;
};

}  // namespace

TEST_CASE("UtcTicksToLocalTime keeps UTC in a UTC zone",
          "[device-access][local-time]") {
  const ScopedZone zone("UTC0");

  const std::optional<LocalTime> local =
      UtcTicksToLocalTime(new_year_2026_ticks);

  REQUIRE(local.has_value());
  CHECK(local->year == 2026);
  CHECK(local->month == 1);
  CHECK(local->day == 1);
  CHECK(local->hour == 0);
  CHECK(local->minute == 0);
}

TEST_CASE("UtcTicksToLocalTime applies the zone offset",
          "[device-access][local-time]") {
  const ScopedZone zone("TST-2");

  const std::optional<LocalTime> local =
      UtcTicksToLocalTime(new_year_2026_ticks);

  REQUIRE(local.has_value());
  CHECK(local->year == 2026);
  CHECK(local->hour == 2);
  CHECK(local->minute == 0);
}

TEST_CASE("UtcTicksToLocalTime crosses midnight into the next day",
          "[device-access][local-time]") {
  const ScopedZone zone("TST-2");

  const std::optional<LocalTime> local =
      UtcTicksToLocalTime(new_year_2026_ticks - minute_ticks);

  REQUIRE(local.has_value());
  CHECK(local->year == 2026);
  CHECK(local->month == 1);
  CHECK(local->day == 1);
  CHECK(local->hour == 1);
  CHECK(local->minute == 59);
}

TEST_CASE("UtcTicksToLocalTime truncates the seconds",
          "[device-access][local-time]") {
  const ScopedZone zone("UTC0");

  const std::optional<LocalTime> local = UtcTicksToLocalTime(
      new_year_2026_ticks + minute_ticks + (59 * second_ticks));

  REQUIRE(local.has_value());
  CHECK(local->minute == 1);
}

TEST_CASE("UtcTicksToLocalTime reaches the FILETIME epoch",
          "[device-access][local-time]") {
  const ScopedZone zone("UTC0");

  const std::optional<LocalTime> local = UtcTicksToLocalTime(0);

  REQUIRE(local.has_value());
  CHECK(local->year == 1601);
  CHECK(local->month == 1);
  CHECK(local->day == 1);
}
