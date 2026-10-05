#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsCompare
{

// 100 ns ticks per second: a Win32 FILETIME's own unit, and this tool's
// common currency for every timestamp, whatever API it came from.
inline constexpr ULONGLONG ticks_per_second = 10'000'000ULL;
// Seconds the Unix epoch (1970-01-01) follows FILETIME's (1601-01-01).
inline constexpr ULONGLONG unix_epoch_offset_seconds = 11'644'473'600ULL;
// Bits in a DWORD: where a FILETIME's high half starts.
inline constexpr unsigned dword_bits = 32;
// Nanoseconds in one 100 ns FILETIME tick.
inline constexpr ULONGLONG nanos_per_tick = 100;

// Combines a FILETIME already in UTC into its 100 ns tick count since
// 1601-01-01.
[[nodiscard]] inline ULONGLONG
    FiletimeToUtcTicks(const FILETIME& file_time) noexcept
{
  return (static_cast<ULONGLONG>(file_time.dwHighDateTime) << dword_bits) |
         file_time.dwLowDateTime;
}

#ifdef _WIN32
// NtfsBrowser's own timestamps come back as LOCAL time on Windows
// (AttrStdInfo::UTC2Local() converts them); this reverses that so every
// method is compared on the same UTC basis.
[[nodiscard]] inline ULONGLONG
    LibraryFiletimeToUtcTicks(const FILETIME& local) noexcept
{
  FILETIME utc{};
  if (LocalFileTimeToFileTime(&local, &utc) == 0)
  {
    return FiletimeToUtcTicks(local);
  }
  return FiletimeToUtcTicks(utc);
}
#else
// UTC2Local() is a no-op off Windows: the library already returns UTC there.
[[nodiscard]] inline ULONGLONG
    LibraryFiletimeToUtcTicks(const FILETIME& already_utc) noexcept
{
  return FiletimeToUtcTicks(already_utc);
}
#endif

// A POSIX time_t/timespec pair (always UTC) to FILETIME-style UTC ticks.
[[nodiscard]] inline ULONGLONG
    SecondsNanosToUtcTicks(long long seconds, long long nanoseconds) noexcept
{
  return (static_cast<ULONGLONG>(seconds) + unix_epoch_offset_seconds) *
             ticks_per_second +
         static_cast<ULONGLONG>(nanoseconds) / nanos_per_tick;
}

}  // namespace NtfsCompare
