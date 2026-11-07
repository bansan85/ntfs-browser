#pragma once

#include <cstdint>
#include <utility>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Bench {

// User and kernel CPU time of the calling thread, in nanoseconds. Both are 0
// when the system does not say. Where the system keeps no split, as on Linux
// for a thread, the kernel part is 0.
[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> ThreadTimes() noexcept;

// User and kernel CPU time of the whole process, in nanoseconds. Both are 0
// when the system does not say.
[[nodiscard]] std::pair<std::uint64_t, std::uint64_t> ProcessTimes() noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
