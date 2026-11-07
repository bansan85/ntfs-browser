#include <cstdint>
#include <ctime>
#include <utility>

#include <sys/resource.h>

#include <ntfs-browser/namespace.h>

#include "bench/process-times.h"

namespace NTFS_BROWSER_NAMESPACE::Bench {

namespace {

// Nanoseconds in a second.
constexpr std::uint64_t ns_per_second = 1'000'000'000;

std::uint64_t TimespecNs(const timespec& time) noexcept {
  return (static_cast<std::uint64_t>(time.tv_sec) * ns_per_second) +
         static_cast<std::uint64_t>(time.tv_nsec);
}

std::uint64_t TimevalNs(const timeval& time) noexcept {
  constexpr std::uint64_t ns_per_microsecond = 1000;
  return (static_cast<std::uint64_t>(time.tv_sec) * ns_per_second) +
         (static_cast<std::uint64_t>(time.tv_usec) * ns_per_microsecond);
}

}  // namespace

std::pair<std::uint64_t, std::uint64_t> ThreadTimes() noexcept {
  timespec time{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
    return {0, 0};
  }
  return {TimespecNs(time), 0};
}

std::pair<std::uint64_t, std::uint64_t> ProcessTimes() noexcept {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return {0, 0};
  }
  return {TimevalNs(usage.ru_utime), TimevalNs(usage.ru_stime)};
}

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
