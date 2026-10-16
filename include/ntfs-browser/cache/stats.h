#pragma once

#include <cstdint>
#include <limits>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Cache {

// Default for VolumeOptions::max_cache_bytes. 256 MiB is 4096 blocks: a large
// working set, and still safe in a 32-bit process.
inline constexpr std::uint64_t default_max_cache_bytes = 256ULL * 1024 * 1024;

// Largest value of VolumeOptions::max_cache_bytes. It removes the limit: the
// cache keeps everything, and it never frees a block by itself.
// Parenthesised, so a windows.h that defines max() cannot break it.
inline constexpr std::uint64_t unlimited_bytes =
    (std::numeric_limits<std::uint64_t>::max)();

// Memory the cache of one volume holds. Both fields are zero under NoCache.
struct Stats {
  // Every cached block, in use or not, plus every live stitched buffer. This
  // is the figure the ceiling limits.
  std::uint64_t held_bytes{0};
  // The part of held_bytes that a live Cache::View holds. The cache cannot
  // free it.
  std::uint64_t in_use_bytes{0};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Cache
