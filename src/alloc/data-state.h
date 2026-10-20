#pragma once

#include <span>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Alloc {

// The data state of a stream from the state of each of its extents. Reallocated
// when any extent is Allocated, even if the stream is damaged elsewhere: that
// is a positive finding. Else Bad when any extent is Bad, which is a positive
// finding too. Else Unknown when the stream is not complete, or an extent is
// out of range or could not be judged. Else Intact, which includes a stream
// with no extent at all.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY DataState FoldDataState(
    bool complete, std::span<const ClusterState> extents) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
