#include "alloc/data-state.h"

#include <algorithm>
#include <span>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Alloc {

DataState FoldDataState(bool complete,
                        std::span<const ClusterState> extents) noexcept {
  if (std::ranges::find(extents, ClusterState::Allocated) != extents.end()) {
    return DataState::Reallocated;
  }
  if (std::ranges::find(extents, ClusterState::Bad) != extents.end()) {
    return DataState::Bad;
  }
  const bool unjudged = std::ranges::any_of(extents, [](ClusterState state) {
    return state == ClusterState::OutOfRange || state == ClusterState::Unknown;
  });
  return (!complete || unjudged) ? DataState::Unknown : DataState::Intact;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
