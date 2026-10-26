#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Alloc {

// How much of the per-cluster account a scan keeps. Off is the default: the
// scan then reads exactly what it reads without the feature. Counts gives each
// concerned entry a ClusterTally. CountsAndMap adds, for each $DATA stream, the
// pieces of its cluster map in file order, which cost memory in proportion to
// the fragments of the concerned files.
enum class ClusterAccountLevel : std::uint8_t { Off, Counts, CountsAndMap };

// How far a scan reads the clusters it presumes intact, to look at their
// content. Off reads none. Sample reads the first intact cluster of each
// concerned stream, the one with the lowest VCN. Full reads every intact
// cluster. An EFS stream is never read: its ciphertext proves nothing.
enum class DataCheckMode : std::uint8_t { Off, Sample, Full };

// What is known of one cluster of a stream, or of one run of them. Each real
// cluster gets exactly one value, the first rule that matches. The first group
// is for a freed file, the second for a live file that is damaged or salvaged,
// and the third for what only a look at the content tells. Invalid, Bad and
// Unknown serve both kinds.
//
// A freed file only claims the clusters a scan could still read: the extension
// records another file reused, and an $ATTRIBUTE_LIST another file overwrote,
// are not read, and their clusters are no part of the file.
enum class ClusterCondition : std::uint8_t {
  // The run has no usable LCN: it lies past the volume, overflows, or is
  // longer than 2^31 clusters, or it lies past where a cut run list ends.
  // Counts the clusters the stream declares.
  Invalid,
  // Listed by the $Bad stream of $BadClus.
  Bad,
  // Past the stream's own valid data length: the file never wrote it, and
  // reads it as zeros. Nothing to recover.
  NeverWritten,
  // Claimed by a live record, within that record's valid data length: the
  // content is lost.
  ReallocatedWritten,
  // Claimed by a live record, but beyond that record's valid data length:
  // allocated, not written yet, so the old content is probably still there.
  ReallocatedUnwritten,
  // Claimed by another freed record that was freed later, within its valid
  // data length. Later means the higher $LogFile sequence number in the header
  // of the file record.
  OverwrittenThenFreed,
  // Claimed by another freed record, and the sequence numbers cannot order the
  // two: they are equal, or one is zero.
  Contested,
  // $Bitmap says allocated, but no record the scan read claims the cluster.
  AllocatedUnknownOwner,
  // $Bitmap cannot answer for it.
  Unknown,
  // Free, claimed by no other record, within the stream's valid data length,
  // not bad. A presumption, not a proof.
  Intact,
  // A cluster of a compressed stream that is intact itself, but whose
  // compression unit holds a cluster in another state: the unit no longer
  // decompresses. A hole of the unit is marked so in the map.
  LostWithUnit,
  // Live file: allocated, and claimed by this record only.
  Consistent,
  // Live file: $Bitmap says the cluster is free.
  FreeButClaimed,
  // Live file: another live record claims it too.
  CrossLinked,
  // A sparse hole. Not a cluster on the disk: the tally counts them apart.
  Sparse,
  // Content check: the cluster was presumed intact and reads all zeros, as
  // after TRIM or a wipe.
  Zeroed,
  // Content check: the cluster was presumed intact and starts a sector with
  // FILE, INDX, RCRD or RSTR: NTFS reused it for its own metadata.
  OverwrittenByMetadata,
  // Content check: the cluster was presumed intact, was read, and looks like
  // data.
  IntactChecked
};

// The number of values of ClusterCondition.
inline constexpr std::size_t cluster_condition_count = 18;

// A counter per ClusterCondition. A cluster is in exactly one of them. The
// sparse holes of a stream are counted under Sparse, and are no cluster.
struct ClusterTally {
  // Indexed by the value of the condition.
  std::array<ULONGLONG, cluster_condition_count> counts{};

  // Clusters counted under condition.
  [[nodiscard]] ULONGLONG Count(ClusterCondition condition) const {
    return counts.at(static_cast<std::size_t>(condition));
  }

  // Adds clusters to the counter of condition.
  void Add(ClusterCondition condition, ULONGLONG clusters) {
    counts.at(static_cast<std::size_t>(condition)) += clusters;
  }

  // Adds every counter of other to this tally.
  void Add(const ClusterTally& other) {
    for (std::size_t i = 0; i < counts.size(); i++) {
      counts.at(i) += other.counts.at(i);
    }
  }

  // Whether no counter holds a cluster.
  [[nodiscard]] bool Empty() const noexcept {
    for (const ULONGLONG count : counts) {
      if (count != 0) {
        return false;
      }
    }
    return true;
  }
};

// One stretch of a stream's cluster map. The pieces of a map are in VCN order,
// cover the stream with no gap, and an adjacent piece of the same condition is
// merged into it, so a piece may span several fragments.
struct ClusterPiece {
  // First virtual cluster of the piece.
  ULONGLONG first_vcn{0};
  // Clusters of the piece.
  ULONGLONG clusters{0};
  // LCN of the first cluster. Empty for a sparse hole, and for the clusters a
  // cut run list left without an address.
  std::optional<ULONGLONG> lcn;
  // What is known of every cluster of the piece.
  ClusterCondition condition{ClusterCondition::Unknown};
};

// The cluster map of one $DATA stream, in file order. For a compressed stream
// the VCNs are those of the uncompressed bytes, and a damaged compression unit
// marks all of its VCNs.
struct StreamClusterMap {
  // The stream's name. Empty for the unnamed stream.
  std::wstring name;
  // Real size of the stream in bytes, and the part of it that was written.
  ULONGLONG data_size{0};
  ULONGLONG initialized_size{0};
  // The map: in VCN order, with no gap.
  std::vector<ClusterPiece> pieces;
};

// The per-cluster account of one entry, over all of its non-resident $DATA
// streams, named ones included.
struct ClusterAccount {
  // The clusters of every stream of the entry, by condition.
  ClusterTally tally;
  // The map of each stream, in the order of the record. Empty unless the scan
  // ran at ClusterAccountLevel::CountsAndMap.
  std::vector<StreamClusterMap> streams;
};

// A stretch of a stream, in bytes, with what is known of it.
struct StreamByteRange {
  // First byte of the stretch, and its length. The last range ends at the
  // size of the stream, not at the end of its last cluster.
  ULONGLONG offset{0};
  ULONGLONG length{0};
  // The condition of the piece of the map the stretch comes from.
  ClusterCondition condition{ClusterCondition::Unknown};
  // The stretch reads as zeros, as Windows returns it: a sparse hole, or a
  // stretch the file never wrote. That is not damage.
  bool reads_as_zeros{false};
  // The stretch is probably not the file's own bytes: see IsDamaged().
  bool damaged{false};
};

// Whether a cluster of this condition is probably not the file's own content.
// Not damaged: Intact, IntactChecked, Consistent, Sparse and NeverWritten, the
// last two because they read as zeros and always did.
[[nodiscard]] NTFS_BROWSER_EXPORT bool
    IsDamaged(ClusterCondition condition) noexcept;

// A short lower-case name of a condition, for a listing.
[[nodiscard]] NTFS_BROWSER_EXPORT const char*
    ConditionName(ClusterCondition condition) noexcept;

// Turns a cluster map into byte ranges of the stream, in file order: piece
// [vcn, vcn + n) becomes [vcn * cluster_size, min((vcn + n) * cluster_size,
// map.data_size)). A piece that starts at or past the end of the stream is
// left out. The ranges are the pieces of the map, not merged again.
[[nodiscard]] NTFS_BROWSER_EXPORT std::vector<StreamByteRange>
    ToByteRanges(const StreamClusterMap& map, ULONGLONG cluster_size);

}  // namespace NTFS_BROWSER_NAMESPACE::Alloc
