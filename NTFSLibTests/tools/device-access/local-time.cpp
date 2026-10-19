#include "local-time.h"

#include <ntfs-browser/win-types.h>

#include <optional>

#ifndef _WIN32
  #include <ctime>
#endif

namespace NtfsDevice {

namespace {

#ifdef _WIN32

// Bits in a DWORD: where a FILETIME's high half starts.
constexpr unsigned dword_bits = 32;

#else

// 100 ns ticks per second: a FILETIME's own unit.
constexpr ULONGLONG ticks_per_second = 10'000'000ULL;
// Seconds the Unix epoch (1970-01-01) follows FILETIME's (1601-01-01).
constexpr LONGLONG unix_epoch_offset_seconds = 11'644'473'600LL;
// Years struct tm counts from, and its month base (January is 0).
constexpr int tm_year_base = 1900;
constexpr int tm_month_base = 1;

#endif

}  // namespace

std::optional<LocalTime> UtcTicksToLocalTime(ULONGLONG ticks) {
#ifdef _WIN32
  FILETIME utc{};
  utc.dwLowDateTime = static_cast<DWORD>(ticks);
  utc.dwHighDateTime = static_cast<DWORD>(ticks >> dword_bits);

  FILETIME local_file_time{};
  SYSTEMTIME local{};
  if (FileTimeToLocalFileTime(&utc, &local_file_time) == 0 ||
      FileTimeToSystemTime(&local_file_time, &local) == 0) {
    return std::nullopt;
  }
  return LocalTime{.year = local.wYear,
                   .month = local.wMonth,
                   .day = local.wDay,
                   .hour = local.wHour,
                   .minute = local.wMinute};
#else
  const auto seconds = static_cast<LONGLONG>(ticks / ticks_per_second) -
                       unix_epoch_offset_seconds;
  const auto unix_time = static_cast<std::time_t>(seconds);

  std::tm local{};
  if (localtime_r(&unix_time, &local) == nullptr) {
    return std::nullopt;
  }
  return LocalTime{.year = static_cast<unsigned>(local.tm_year + tm_year_base),
                   .month = static_cast<unsigned>(local.tm_mon + tm_month_base),
                   .day = static_cast<unsigned>(local.tm_mday),
                   .hour = static_cast<unsigned>(local.tm_hour),
                   .minute = static_cast<unsigned>(local.tm_min)};
#endif
}

}  // namespace NtfsDevice
