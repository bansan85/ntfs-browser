#include "alloc/bad-cluster-list.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

namespace {

// Ceiling a test asked for. 0 means max_bad_extents.
size_t ceiling_override = 0;

// First cluster past the extent, saturated when it would wrap past 2^64.
ULONGLONG EndOf(const Extent& extent) noexcept {
  return extent.lcn > std::numeric_limits<ULONGLONG>::max() - extent.clusters
             ? std::numeric_limits<ULONGLONG>::max()
             : extent.lcn + extent.clusters;
}

// Clamps every extent to cluster_count and drops the empty ones. Returns true
// when an extent was dropped or cut. cluster_count 0 clamps nothing beyond the
// 2^64 saturation.
bool ClampToVolume(std::vector<Extent>& extents, ULONGLONG cluster_count) {
  bool clipped = false;
  size_t kept = 0;
  for (const Extent& extent : extents) {
    if (extent.clusters == 0) {
      continue;
    }
    const ULONGLONG wrapped_end = EndOf(extent);
    ULONGLONG end = wrapped_end;
    if (cluster_count != 0) {
      if (extent.lcn >= cluster_count) {
        clipped = true;
        continue;
      }
      end = std::min(end, cluster_count);
    }
    clipped = clipped || end < wrapped_end;
    extents[kept++] = {.lcn = extent.lcn, .clusters = end - extent.lcn};
  }
  extents.resize(kept);
  return clipped;
}

// Sorts by cluster, then joins extents that touch or overlap, in place.
void SortAndMerge(std::vector<Extent>& extents) {
  std::ranges::sort(extents, [](const Extent& left, const Extent& right) {
    return left.lcn != right.lcn ? left.lcn < right.lcn
                                 : left.clusters < right.clusters;
  });
  size_t kept = 0;
  for (const Extent& extent : extents) {
    if (kept != 0) {
      Extent& last = extents[kept - 1];
      const ULONGLONG last_end = EndOf(last);
      // The next extent starts inside the last one or right after it.
      if (extent.lcn <= last_end) {
        last.clusters = std::max(last_end, EndOf(extent)) - last.lcn;
        continue;
      }
    }
    extents[kept++] = extent;
  }
  extents.resize(kept);
}

}  // namespace

void SetBadExtentCeilingForTests(size_t ceiling) { ceiling_override = ceiling; }

size_t BadExtentCeiling() {
  return ceiling_override != 0 ? ceiling_override : max_bad_extents;
}

BadClusterList::BadClusterList(BadClusters list) noexcept
    : list_(std::move(list)) {}

std::unique_ptr<BadClusterList> BadClusterList::Build(StreamExtents extents,
                                                      ULONGLONG cluster_count,
                                                      bool recover,
                                                      size_t ceiling) {
  try {
    if (!extents.complete) {
      Log::Recoverable(recover, "$BadClus run list is incomplete: {}",
                       recover ? "the clusters past it are judged by the "
                                 "bitmap alone"
                               : "the list is rejected");
      if (!recover) {
        return nullptr;
      }
    }

    if (ClampToVolume(extents.extents, cluster_count)) {
      Log::Recoverable(recover,
                       "$BadClus names clusters past the volume: they are "
                       "ignored");
    }
    SortAndMerge(extents.extents);

    BadClusters list;
    if (extents.extents.size() > ceiling) {
      Log::Recoverable(
          recover, "$BadClus lists more than {} extents: {}", ceiling,
          recover ? "the lowest are kept" : "the list is rejected");
      if (!recover) {
        return nullptr;
      }
      extents.extents.resize(ceiling);
      list.truncated = true;
    }
    extents.extents.shrink_to_fit();
    list.extents = std::move(extents.extents);
    return std::unique_ptr<BadClusterList>(new BadClusterList(std::move(list)));
  } catch (const std::exception& e) {
    Log::Exception(e);
    return nullptr;
  }
}

const BadClusters& BadClusterList::Get() const noexcept { return list_; }

std::span<const Extent>
    BadClusterList::Overlapping(ULONGLONG first,
                                ULONGLONG last) const noexcept {
  if (first >= last) {
    return {};
  }
  const std::vector<Extent>& extents = list_.extents;
  // The first extent that ends after first. Extents are sorted and do not
  // overlap, so their ends are sorted too.
  const auto begin =
      std::ranges::partition_point(extents, [first](const Extent& extent) {
        return EndOf(extent) <= first;
      });
  // The first extent that starts at or after last.
  const auto end = std::ranges::partition_point(
      begin, extents.end(),
      [last](const Extent& extent) { return extent.lcn < last; });
  // An iterator at the end MUST NOT be dereferenced, so go through the pointer.
  return {extents.data() + (begin - extents.begin()),
          static_cast<size_t>(end - begin)};
}

ULONGLONG BadClusterList::CountIn(ULONGLONG first,
                                  ULONGLONG last) const noexcept {
  ULONGLONG count = 0;
  for (const Extent& extent : Overlapping(first, last)) {
    count += std::min(EndOf(extent), last) - std::max(extent.lcn, first);
  }
  return count;
}

size_t BadClusterList::BytesHeld() const noexcept {
  return list_.extents.capacity() * sizeof(Extent);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
