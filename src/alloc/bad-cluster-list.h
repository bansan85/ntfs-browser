#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <span>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

// Most bad extents a loaded list keeps. A $Bad stream on a Windows volume with
// 4 KiB file records holds at most 5 168 739: 6551 attribute-list segments (the
// 256 KiB limit of Windows, at 40 bytes an entry) of 789 extents each. 5 x 2^20
// rounds that up. At 16 bytes an extent the list then takes 80 MiB, so a forged
// volume cannot make it grow without bound.
inline constexpr size_t max_bad_extents = size_t{5} * 1024 * 1024;

// Test hook: the ceiling new lists use. 0 restores max_bad_extents. A test
// lowers it to cut a small list.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SetBadExtentCeilingForTests(size_t ceiling);

// The ceiling new lists use: max_bad_extents, or the value a test set.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t BadExtentCeiling();

// The bad extents of a volume, ready to be asked about. They are sorted by
// cluster, merged, and clamped to the volume, so a query is a binary search and
// a short walk. It does not know where the extents come from. Read-only once
// built. Not thread safe.
class NTFS_BROWSER_EXPORT_TESTS_ONLY BadClusterList {
 public:
  // Builds the list from the real runs of a $Bad stream. cluster_count is the
  // volume's cluster count, or 0 when unknown, which clamps nothing. Returns
  // null when the list is unusable: the stream is damaged and recover is off,
  // there are more than ceiling extents and recover is off, or building failed.
  // With recover on, a damaged stream keeps the runs that parsed and a list
  // over the ceiling keeps its lowest extents and is flagged truncated.
  [[nodiscard]] static std::unique_ptr<BadClusterList>
      Build(StreamExtents extents, ULONGLONG cluster_count, bool recover,
            size_t ceiling = BadExtentCeiling());

  // The extents, with the truncation flag.
  [[nodiscard]] const BadClusters& Get() const noexcept;

  // The extents that share a cluster with [first, last), in order. Empty for an
  // empty range.
  [[nodiscard]] std::span<const Extent>
      Overlapping(ULONGLONG first, ULONGLONG last) const noexcept;

  // Bad clusters inside [first, last).
  [[nodiscard]] ULONGLONG CountIn(ULONGLONG first,
                                  ULONGLONG last) const noexcept;

  // Bytes the extents hold right now, capacity included.
  [[nodiscard]] size_t BytesHeld() const noexcept;

 private:
  explicit BadClusterList(BadClusters list) noexcept;

  BadClusters list_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
