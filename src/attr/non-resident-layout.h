#pragma once

#include <optional>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "attr/run-list.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Data {

struct HeaderNonResident;

}  // namespace Data

namespace Attr {

// Answers the layout questions of one non-resident attribute: where its
// clusters are, which byte ranges are real, and whether the volume agrees with
// the runs. It reads nothing from the disk, except the cluster bitmap in
// CheckClusterAllocation(). The attribute owns the data. This class only looks
// at it, so the attribute MUST outlive it.
template <Cache::Strategy S>
class NonResidentLayout {
 public:
  // What the layout needs from its attribute. The references stay valid for
  // the life of the attribute: it is never moved, and runs grows in place.
  struct Source {
    const NtfsVolume<S>& volume;
    const Data::HeaderNonResident& header;
    const RunList& runs;
    DWORD cluster_size;
    // Clusters per compression unit; 0 means uncompressed.
    ULONGLONG comp_unit_clusters;
    Attr::Type type;
    WORD attr_flags;
    // Whether the record was freed, and its record number. The cluster check
    // needs both: a freed record points at free clusters by nature, and the
    // volume's own metadata is not checked.
    bool record_deleted;
    std::optional<ULONGLONG> record_number;
  };

  // Keeps the source. Nothing is copied or read.
  explicit NonResidentLayout(const Source& source) noexcept;

  // The runs that map clusters, sparse runs left out. The list is complete
  // when it reaches the allocated size the first record declares.
  [[nodiscard]] Alloc::StreamExtents GetExtents() const;

  // The real stretches in bytes, each cut at the data size. The list is
  // complete on the same terms as GetExtents().
  [[nodiscard]] Alloc::StreamRanges GetRealRanges() const;

  // The runs as decoded, sparse holes included, with the sizes of the first
  // part of the stream.
  [[nodiscard]] Alloc::StreamLayout GetLayout() const;

  // The runs of real clusters that a read of length bytes at offset touches,
  // in order, the partial clusters at both ends included. A sparse part has
  // none, and neither has a compressed stream, which reads whole units.
  // Reads nothing.
  [[nodiscard]] std::vector<Alloc::Extent>
      ExtentsOfRange(ULONGLONG offset, ULONGLONG length) const;

  // With recover_errors on, logs once if a cluster of this live data stream
  // is free in the cluster bitmap, and once if one is marked bad in $BadClus.
  // Runs once per attribute. Never changes what a read returns.
  void CheckClusterAllocation() const;

 private:
  Source source_;
  // Set by the first CheckClusterAllocation(): the check runs once.
  mutable bool alloc_checked_{false};
};  // NonResidentLayout

}  // namespace Attr

}  // namespace NTFS_BROWSER_NAMESPACE
