#include "alloc/cluster-account.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

bool IsDamaged(ClusterCondition condition) noexcept {
  // The conditions that mean the content can be read as the file wrote it.
  constexpr std::array<ClusterCondition, 5> undamaged{
      ClusterCondition::Intact, ClusterCondition::IntactChecked,
      ClusterCondition::Consistent, ClusterCondition::Sparse,
      ClusterCondition::NeverWritten};
  return std::ranges::find(undamaged, condition) == undamaged.end();
}

const char* ConditionName(ClusterCondition condition) noexcept {
  // Indexed by the value of the condition, so in the order of the enum.
  constexpr std::array<const char*, cluster_condition_count> names{
      "invalid",
      "bad",
      "never-written",
      "reallocated-written",
      "reallocated-unwritten",
      "overwritten-then-freed",
      "contested",
      "allocated-unknown-owner",
      "unknown",
      "intact",
      "lost-with-unit",
      "consistent",
      "free-but-claimed",
      "cross-linked",
      "sparse",
      "zeroed",
      "overwritten-by-metadata",
      "intact-checked"};
  const auto index = static_cast<std::size_t>(condition);
  return index < names.size() ? names.at(index) : "unknown";
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
