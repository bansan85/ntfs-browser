#include <cstdint>
#include <utility>

#include <windows.h>

#include <ntfs-browser/namespace.h>

#include "bench/process-times.h"

namespace NTFS_BROWSER_NAMESPACE::Bench {

namespace {

// Ticks of a FILETIME are 100 nanoseconds.
constexpr std::uint64_t ns_per_filetime_tick = 100;

std::uint64_t FileTimeNs(const FILETIME& time) noexcept {
  return ((static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) |
          time.dwLowDateTime) *
         ns_per_filetime_tick;
}

}  // namespace

std::pair<std::uint64_t, std::uint64_t> ThreadTimes() noexcept {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) ==
      FALSE) {
    return {0, 0};
  }
  return {FileTimeNs(user), FileTimeNs(kernel)};
}

std::pair<std::uint64_t, std::uint64_t> ProcessTimes() noexcept {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ==
      FALSE) {
    return {0, 0};
  }
  return {FileTimeNs(user), FileTimeNs(kernel)};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
