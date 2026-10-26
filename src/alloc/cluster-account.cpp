#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <limits>
#include <vector>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Alloc {

bool IsDamaged(ClusterCondition condition) noexcept {
  switch (condition) {
    case ClusterCondition::Intact:
    case ClusterCondition::IntactChecked:
    case ClusterCondition::Consistent:
    case ClusterCondition::Sparse:
    case ClusterCondition::NeverWritten:
      return false;
    case ClusterCondition::Invalid:
    case ClusterCondition::Bad:
    case ClusterCondition::ReallocatedWritten:
    case ClusterCondition::ReallocatedUnwritten:
    case ClusterCondition::OverwrittenThenFreed:
    case ClusterCondition::Contested:
    case ClusterCondition::AllocatedUnknownOwner:
    case ClusterCondition::Unknown:
    case ClusterCondition::LostWithUnit:
    case ClusterCondition::FreeButClaimed:
    case ClusterCondition::CrossLinked:
    case ClusterCondition::Zeroed:
    case ClusterCondition::OverwrittenByMetadata:
      return true;
  }
  return true;
}

const char* ConditionName(ClusterCondition condition) noexcept {
  switch (condition) {
    case ClusterCondition::Invalid:
      return "invalid";
    case ClusterCondition::Bad:
      return "bad";
    case ClusterCondition::NeverWritten:
      return "never-written";
    case ClusterCondition::ReallocatedWritten:
      return "reallocated-written";
    case ClusterCondition::ReallocatedUnwritten:
      return "reallocated-unwritten";
    case ClusterCondition::OverwrittenThenFreed:
      return "overwritten-then-freed";
    case ClusterCondition::Contested:
      return "contested";
    case ClusterCondition::AllocatedUnknownOwner:
      return "allocated-unknown-owner";
    case ClusterCondition::Unknown:
      return "unknown";
    case ClusterCondition::Intact:
      return "intact";
    case ClusterCondition::LostWithUnit:
      return "lost-with-unit";
    case ClusterCondition::Consistent:
      return "consistent";
    case ClusterCondition::FreeButClaimed:
      return "free-but-claimed";
    case ClusterCondition::CrossLinked:
      return "cross-linked";
    case ClusterCondition::Sparse:
      return "sparse";
    case ClusterCondition::Zeroed:
      return "zeroed";
    case ClusterCondition::OverwrittenByMetadata:
      return "overwritten-by-metadata";
    case ClusterCondition::IntactChecked:
      return "intact-checked";
  }
  return "unknown";
}

std::vector<StreamByteRange> ToByteRanges(const StreamClusterMap& map,
                                          ULONGLONG cluster_size) {
  constexpr ULONGLONG max = (std::numeric_limits<ULONGLONG>::max)();
  // Saturating product: a forged cluster count must not wrap.
  const auto bytes_of = [cluster_size](ULONGLONG clusters) {
    return cluster_size != 0 && clusters > max / cluster_size
               ? max
               : clusters * cluster_size;
  };

  std::vector<StreamByteRange> ranges;
  ranges.reserve(map.pieces.size());
  for (const ClusterPiece& piece : map.pieces) {
    const ULONGLONG offset = bytes_of(piece.first_vcn);
    if (offset >= map.data_size) {
      continue;
    }
    const ULONGLONG end =
        (std::min)(map.data_size, offset > max - bytes_of(piece.clusters)
                                      ? max
                                      : offset + bytes_of(piece.clusters));
    ranges.push_back(
        {.offset = offset,
         .length = end - offset,
         .condition = piece.condition,
         .reads_as_zeros = piece.condition == ClusterCondition::Sparse ||
                           piece.condition == ClusterCondition::NeverWritten,
         .damaged = IsDamaged(piece.condition)});
  }
  return ranges;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
