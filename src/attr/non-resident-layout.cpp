#include "attr/non-resident-layout.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "attr/flags.h"
#include "attr/run-list.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "data/header-non-resident.h"
#include "data/run-entry.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

namespace {

// The clusters of one kind found in the runs of a stream, and the LCN of the
// first run that holds some.
struct ClusterTally {
  ULONGLONG clusters{0};
  std::optional<ULONGLONG> first_lcn;

  // Counts the clusters of a run that starts at lcn. A run with none is left
  // out.
  void Add(ULONGLONG run_clusters, ULONGLONG lcn) {
    if (run_clusters == 0) {
      return;
    }
    clusters += run_clusters;
    if (!first_lcn) {
      first_lcn = lcn;
    }
  }
};

}  // namespace

template <Cache::Strategy S>
NonResidentLayout<S>::NonResidentLayout(const Source& source) noexcept
    : source_(source) {}

// Looks, once, at the clusters of a live data stream in the cluster bitmap and
// in the bad list. A run on a free cluster, or on a cluster marked bad, is a
// sign of damage: the volume and its metadata disagree. Each is logged apart,
// once per stream, and a cluster is in one count only. It only matters when
// recovering, so strict mode reads nothing more. The volume's own metadata
// (records below Mft::Idx::User, $Bitmap and $BadClus included) and freed
// records are left out: a freed file's clusters are free by nature, and
// checking $Bitmap against itself would loop.
template <Cache::Strategy S>
void NonResidentLayout<S>::CheckClusterAllocation() const {
  if (alloc_checked_) {
    return;
  }
  alloc_checked_ = true;

  if (!source_.volume.GetOptions().recover_errors || source_.record_deleted ||
      source_.type != Attr::Type::Data || !source_.record_number) {
    return;
  }
  const ULONGLONG record = *source_.record_number & Mft::mft_record_number_mask;
  if (record < static_cast<ULONGLONG>(Mft::Idx::User)) {
    return;
  }

  try {
    const Alloc::StreamExtents extents = GetExtents();
    std::vector<Alloc::RangeQuery> queries;
    queries.reserve(extents.extents.size());
    for (const Alloc::Extent& extent : extents.extents) {
      queries.push_back({.lcn = extent.lcn, .clusters = extent.clusters});
    }
    source_.volume.GetRangeStates(queries);

    ClusterTally free_found;
    ClusterTally bad_found;
    for (const Alloc::RangeQuery& query : queries) {
      free_found.Add(query.free_clusters, query.lcn);
      bad_found.Add(query.bad_clusters, query.lcn);
    }
    if (free_found.first_lcn) {
      Log::Info(
          "Record {}: {} clusters of a data stream are free in $Bitmap, from "
          "the run at LCN {}",
          record, free_found.clusters, *free_found.first_lcn);
    }
    if (bad_found.first_lcn) {
      Log::Info(
          "Record {}: {} clusters of a data stream are marked bad in "
          "$BadClus, from the run at LCN {}",
          record, bad_found.clusters, *bad_found.first_lcn);
    }
  } catch (const std::exception& e) {
    Log::Exception(e);
  }
}

template <Cache::Strategy S>
std::vector<Alloc::Extent>
    NonResidentLayout<S>::ExtentsOfRange(ULONGLONG offset,
                                         ULONGLONG length) const {
  const ULONGLONG cluster_size = source_.cluster_size;
  if (length == 0 || cluster_size == 0 || source_.comp_unit_clusters != 0 ||
      offset > std::numeric_limits<ULONGLONG>::max() - length) {
    return {};
  }

  // The clusters that hold a byte of the range, as ReadDataBounded() reads
  // them: the partial ones at both ends included.
  const ULONGLONG vcn = offset / cluster_size;
  const ULONGLONG end_vcn = (offset + length - 1) / cluster_size + 1;
  // The byte address of an LCN MUST NOT wrap, as in ReadClusters().
  const ULONGLONG max_lcn =
      static_cast<ULONGLONG>(std::numeric_limits<LONGLONG>::max()) /
      cluster_size;
  return source_.runs.ExtentsBetween(vcn, end_vcn, max_lcn);
}

// The runs that map clusters. The list is complete when it reaches the end of
// the allocated size the first record declares. A shorter list means a run was
// lost: recover mode stops at a bad run, and an extension record that failed to
// parse leaves its part of the stream out.
template <Cache::Strategy S>
Alloc::StreamExtents NonResidentLayout<S>::GetExtents() const {
  RunList::ExtentList list = source_.runs.Extents();
  Alloc::StreamExtents extents{.extents = std::move(list.extents),
                               .complete = list.well_formed};

  const ULONGLONG cluster_size = source_.cluster_size;
  if (cluster_size != 0) {
    const ULONGLONG allocated = source_.header.alloc_size;
    const ULONGLONG needed =
        allocated / cluster_size + (allocated % cluster_size != 0 ? 1 : 0);
    if (list.covered_clusters < needed) {
      extents.complete = false;
    }
  }
  return extents;
}

// The real stretches in bytes. Each is cut at the data size, since clusters
// past it are allocated but hold no byte of the stream. The list is complete
// on the same terms as GetExtents().
template <Cache::Strategy S>
Alloc::StreamRanges NonResidentLayout<S>::GetRealRanges() const {
  const RunList::VcnRunList list = source_.runs.RealRanges();
  Alloc::StreamRanges result{.complete = list.well_formed};

  const ULONGLONG cluster_size = source_.cluster_size;
  if (cluster_size == 0) {
    result.complete = false;
    return result;
  }

  const ULONGLONG allocated = source_.header.alloc_size;
  const ULONGLONG needed =
      allocated / cluster_size + (allocated % cluster_size != 0 ? 1 : 0);
  if (list.covered_clusters < needed) {
    result.complete = false;
  }

  const ULONGLONG data_size = source_.header.real_size;
  constexpr ULONGLONG max_offset = std::numeric_limits<ULONGLONG>::max();
  for (const RunList::VcnRun& run : list.runs) {
    if (run.start_vcn > max_offset / cluster_size ||
        run.clusters > max_offset / cluster_size) {
      result.complete = false;
      continue;
    }
    const ULONGLONG offset = run.start_vcn * cluster_size;
    if (offset >= data_size) {
      continue;
    }
    const ULONGLONG length =
        (std::min)(run.clusters * cluster_size, data_size - offset);
    result.ranges.push_back({.offset = offset, .length = length});
  }
  return result;
}

// The runs as the list decoded them, shifted to the VCN this instance starts
// at: a part of a stream that was not merged into its first one keeps its own
// VCNs. Only the first part of a stream has sizes: a continuation reports
// everything it maps as written.
template <Cache::Strategy S>
Alloc::StreamLayout NonResidentLayout<S>::GetLayout() const {
  const Data::HeaderNonResident& header = source_.header;
  Alloc::StreamLayout layout;
  const ULONGLONG shift = header.start_vcn;
  ULONGLONG covered = shift;
  for (const Data::RunEntry& entry : source_.runs.Entries()) {
    if (entry.clusters == 0) {
      layout.well_formed = false;
      continue;
    }
    if (entry.lcn &&
        entry.clusters > std::numeric_limits<ULONGLONG>::max() - *entry.lcn) {
      layout.well_formed = false;
    }
    layout.runs.push_back({.vcn = shift + entry.start_vcn,
                           .clusters = entry.clusters,
                           .lcn = entry.lcn});
    covered = shift + entry.last_vcn + 1;
  }
  layout.covered_end = covered;

  const ULONGLONG cluster_size = source_.cluster_size;
  const bool first_part = header.start_vcn == 0;
  layout.data_size = header.real_size;
  layout.initialized_size = (std::min)(header.ini_size, header.real_size);
  layout.initialized_clusters = (std::numeric_limits<ULONGLONG>::max)();
  if (cluster_size != 0 && first_part) {
    const auto clusters_of = [cluster_size](ULONGLONG bytes) {
      return bytes / cluster_size + (bytes % cluster_size != 0 ? 1 : 0);
    };
    layout.declared_clusters = clusters_of(header.alloc_size);
    layout.initialized_clusters = clusters_of(layout.initialized_size);
  }
  layout.unit_clusters = source_.comp_unit_clusters;
  layout.encrypted = (source_.attr_flags & Attr::flag_encrypted) != 0;
  return layout;
}

template class NonResidentLayout<Cache::Strategy::NoCache>;
template class NonResidentLayout<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
