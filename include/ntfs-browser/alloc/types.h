#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <optional>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Alloc {

// What the cluster bitmap and the bad cluster list say about one cluster, or
// about a range of them. A range is Allocated when any cluster is allocated and
// not bad; else Bad when any is bad; else OutOfRange when any lies past the
// volume's last cluster; else Unknown when the bitmap cannot say for at least
// one; else Free. A positive finding wins over an error. A cluster named by the
// bad list is Bad whatever its bit in the bitmap, which marks it allocated.
// Bad is appended last: a switch over this enum MUST have a case for it.
enum class ClusterState : std::uint8_t {
  Allocated,
  Free,
  OutOfRange,
  Unknown,
  Bad
};

// Whether the bytes of a deleted data stream are probably still the file's own.
// Reallocated: at least one of its clusters is allocated again, so the bytes
// are probably another file's. Bad: none is reallocated, but one is marked bad,
// so the bytes may be lost or unreadable. Intact: none is either. Unknown: the
// bitmap, the bad list or the run list could not say. The order is
// Reallocated, then Bad, then Unknown, then Intact. A strong hint, not a proof:
// a cluster that is free now may have held other data in between. Bad is
// appended last: a switch over this enum MUST have a case for it.
enum class DataState : std::uint8_t { Intact, Reallocated, Unknown, Bad };

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

// A run of real (non-sparse) bytes, at a byte offset of the stream's own
// address space. Unlike Extent, it says where in the stream the bytes sit.
struct ByteRange {
  ULONGLONG offset{0};
  ULONGLONG length{0};
};

// The real byte ranges of one stream, sorted by offset, sparse holes left out.
// complete means what it means in StreamExtents: it is false when the run list
// is shorter than the stream declares, holds a zero-length run, or holds a run
// that overflows 64 bits.
struct StreamRanges {
  std::vector<ByteRange> ranges;
  bool complete{true};
};

// One question of NtfsVolume::GetRangeStates(). The caller fills lcn and
// clusters; the call fills the rest. allocated_clusters and free_clusters count
// the clusters of the range, clamped to the volume, that the bitmap judged
// allocated and free. The others were out of range or unknown. bad_clusters
// counts the clusters of the range, clamped to the volume, that the bad list
// names. They are in neither allocated_clusters nor free_clusters.
struct RangeQuery {
  ULONGLONG lcn{0};
  ULONGLONG clusters{0};
  ClusterState state{ClusterState::Unknown};
  ULONGLONG allocated_clusters{0};
  ULONGLONG free_clusters{0};
  ULONGLONG bad_clusters{0};
};

// Free space as the cluster bitmap counts it. unknown_clusters are the ones it
// could not judge (a bitmap shorter than the volume, a window that failed to
// read): free_clusters counts only the ones it could. bad_clusters are the ones
// the bad list names, and bad_bytes their size (saturated on overflow). A bad
// cluster is in neither free_clusters nor unknown_clusters, whatever its bit in
// the bitmap.
struct SpaceSummary {
  ULONGLONG total_clusters{0};
  ULONGLONG free_clusters{0};
  ULONGLONG free_bytes{0};
  ULONGLONG unknown_clusters{0};
  ULONGLONG bad_clusters{0};
  ULONGLONG bad_bytes{0};
};

// The volume's bad extents, from the $Bad stream of $BadClus: sorted by
// cluster, merged, and clamped to the volume. truncated is true when the list
// was cut at its ceiling of 5 242 880 extents, which only recover mode does:
// the clusters past the last extent are then judged by the bitmap alone. A
// snapshot, read once.
struct BadClusters {
  std::vector<Extent> extents;
  bool truncated{false};
};

// A run of clusters that share one state, as the cluster bitmap and the bad
// list judge them. See NtfsVolume::GetClusterRuns().
struct ClusterRun {
  // First cluster of the run, and its length.
  ULONGLONG lcn{0};
  ULONGLONG clusters{0};
  // What the bitmap and the bad list say of every cluster of the run.
  ClusterState state{ClusterState::Unknown};
};

// One run of a stream's run list: the clusters [vcn, vcn + clusters) of the
// stream, at LCN lcn, or a sparse hole when lcn is empty.
struct LayoutRun {
  // First virtual cluster of the run, and its length.
  ULONGLONG vcn{0};
  ULONGLONG clusters{0};
  // First real cluster of the run. Empty for a sparse hole.
  std::optional<ULONGLONG> lcn;
};

// How one non-resident attribute maps its clusters: every run of its list in
// VCN order, sparse holes included, with what the header says of its size.
// Nothing is read from the disk to build it.
struct StreamLayout {
  // The runs that decoded, with absolute VCNs. A zero-length run is left out.
  std::vector<LayoutRun> runs;
  // Clusters the allocated size declares, rounded up. 0 on a continuation of
  // the stream, which the header of its first part describes.
  ULONGLONG declared_clusters{0};
  // First VCN past the last decoded run, sparse runs included.
  ULONGLONG covered_end{0};
  // Clusters up to the initialized size, rounded up: the VCNs from here on were
  // never written. The maximum value on a continuation, whose header has no
  // initialized size.
  ULONGLONG initialized_clusters{0};
  // Real and initialized sizes in bytes, from the header.
  ULONGLONG data_size{0};
  ULONGLONG initialized_size{0};
  // Clusters per compression unit. 0 for a stream that is not compressed.
  ULONGLONG unit_clusters{0};
  // The attribute carries the encrypted flag.
  bool encrypted{false};
  // False when a run had zero length, or overflowed 64 bits.
  bool well_formed{true};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
