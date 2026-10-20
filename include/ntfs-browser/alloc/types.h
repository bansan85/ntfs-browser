#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Alloc {

// What the cluster bitmap says about one cluster, or about a range of them.
// A range is Allocated when any cluster is; else OutOfRange when any lies past
// the volume's last cluster; else Unknown when the bitmap cannot say for at
// least one; else Free. A positive finding wins over an error.
enum class ClusterState : std::uint8_t { Allocated, Free, OutOfRange, Unknown };

// Whether the bytes of a deleted data stream are probably still the file's own.
// Reallocated: at least one of its clusters is allocated again, so the bytes
// are probably another file's. Intact: none is. Unknown: the bitmap or the run
// list could not say. A strong hint, not a proof: a cluster that is free now
// may have held other data in between.
enum class DataState : std::uint8_t { Intact, Reallocated, Unknown };

// A run of real (non-sparse) clusters.
struct Extent {
  ULONGLONG lcn{0};
  ULONGLONG clusters{0};
};

// The real clusters of one stream. complete is false when the run list is
// shorter than the stream declares, holds a zero-length run, or holds a run
// that overflows 64 bits.
struct StreamExtents {
  std::vector<Extent> extents;
  bool complete{true};
};

// One question of NtfsVolume::GetRangeStates(). The caller fills lcn and
// clusters; the call fills the rest. allocated_clusters and free_clusters count
// the clusters of the range, clamped to the volume, that the bitmap judged
// allocated and free. The others were out of range or unknown.
struct RangeQuery {
  ULONGLONG lcn{0};
  ULONGLONG clusters{0};
  ClusterState state{ClusterState::Unknown};
  ULONGLONG allocated_clusters{0};
  ULONGLONG free_clusters{0};
};

// Free space as the cluster bitmap counts it. unknown_clusters are the ones it
// could not judge (a bitmap shorter than the volume, a window that failed to
// read): free_clusters counts only the ones it could.
struct SpaceSummary {
  ULONGLONG total_clusters{0};
  ULONGLONG free_clusters{0};
  ULONGLONG free_bytes{0};
  ULONGLONG unknown_clusters{0};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
