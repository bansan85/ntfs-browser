#include "attr/non-resident.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "alloc/types.h"
#include "attr/base.h"
#include "attr/compression-units.h"
#include "attr/flags.h"
#include "attr/header-common.h"
#include "attr/type.h"
#include "bench/bench.h"
#include "bench/recorder.h"
#include "cache/strategy.h"
#include "cache/view.h"
#include "data/header-non-resident.h"
#include "data/run-entry.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  #include "efs/efs-context.h"
#endif

namespace NTFS_BROWSER_NAMESPACE::Attr {

template <Cache::Strategy S>
AttrNonResident<S>::AttrNonResident(const HeaderCommon& ahc,
                                    const Io::FileRecord<S>& file_record)
    : Attr::AttrBase<S>(ahc, file_record),
      attr_header_nr_(reinterpret_cast<const Data::HeaderNonResident&>(ahc)),
      runs_(Data::SpannedClusters(attr_header_nr_)) {
  // total_size already covers this field (ParseAttrs()); start_vcn must be
  // unit-aligned or units decode against the window of another unit.
  if (Data::HasCompressedSizeField(attr_header_nr_)) {
    compression_.emplace(typename CompressionUnits<S>::Source{
        .header = attr_header_nr_,
        .runs = runs_,
        .cluster_size = this->GetClusterSize(),
        .raw_reader = [this](ULONGLONG vcn, ULONGLONG clusters,
                             std::span<BYTE> buffer) {
          return ReadVirtualClustersRaw(vcn, clusters, buffer);
        }});
  }

  layout_.emplace(typename NonResidentLayout<S>::Source{
      .volume = this->volume_,
      .header = attr_header_nr_,
      .runs = runs_,
      .cluster_size = this->GetClusterSize(),
      .comp_unit_clusters = compression_ ? compression_->ClustersPerUnit() : 0,
      .type = this->GetAttrType(),
      .attr_flags = this->GetAttrFlags(),
      .record_deleted = file_record.IsDeleted(),
      .record_number = file_record.GetFileReference()});

  ParseDataRun();
}

// Traverse DataRun and append entries to the run list. When recovering,
// stops at the first decode or bounds error, keeping entries parsed before
// it; when strict, the same error instead throws, rejecting the attribute
// outright.
template <Cache::Strategy S>
void AttrNonResident<S>::ParseDataRun() {
  Log::Trace("Parsing Non Resident DataRun");
  Log::Debug("Start VCN = {}, End VCN = {}", attr_header_nr_.start_vcn,
             attr_header_nr_.last_vcn);

  const bool recover = this->volume_.GetOptions().recover_errors;
  const std::span<const BYTE> attr_bytes(
      reinterpret_cast<const BYTE*>(&attr_header_nr_),
      attr_header_nr_.header.total_size);
  const std::span<const BYTE> data_run =
      attr_header_nr_.data_run_offset < attr_bytes.size()
          ? attr_bytes.subspan(attr_header_nr_.data_run_offset)
          : std::span<const BYTE>{};
  runs_.Parse(data_run, Data::VcnSpan(attr_header_nr_), recover);
}

// Read clusters from disk, or sparse data
// *actural = Clusters acturally read
template <Cache::Strategy S>
std::optional<Cache::ReadResult<S>>
    AttrNonResident<S>::ReadClusters(ULONGLONG clusters, ULONGLONG start_lcn,
                                     ULONGLONG offset) const {
  const Bench::Detail::KindScope kind(Bench::ReadKind::Data, true);
  // start_lcn and offset are attacker-controlled. Their sum times the
  // cluster size is a byte address, and MUST NOT wrap past 2^63: a wrapped
  // address is a valid one, so the read would silently hit other clusters.
  const ULONGLONG max_lcn =
      static_cast<ULONGLONG>(std::numeric_limits<LONGLONG>::max()) /
      this->GetClusterSize();
  if (start_lcn > max_lcn || offset > max_lcn - start_lcn) {
    Log::Error("Cannot read cluster with LCN {} + {}: byte address overflows",
               start_lcn, offset);
    return {};
  }
  const ULONGLONG lcn = start_lcn + offset;

  LARGE_INTEGER addr{.QuadPart =
                         static_cast<LONGLONG>(lcn * this->GetClusterSize())};

  std::optional<Cache::ReadResult<S>> buffer;
  try {
    buffer = this->volume_.Read(
        addr, gsl::narrow<DWORD>(clusters * this->GetClusterSize()));
  } catch (const std::exception& e) {
    Log::Error("Cannot read cluster with LCN {}", lcn);
    Log::Exception(e);
    return {};
  }

  if (!buffer) {
    Log::Error("Cannot read cluster with LCN {}", lcn);
    return {};
  }

  Log::Trace("Successfully read {} clusters from LCN {}", clusters, lcn);
  return buffer;
}

// Number of virtual clusters this attribute describes, merged instances
// included.
template <Cache::Strategy S>
ULONGLONG AttrNonResident<S>::TotalClusters() const noexcept {
  return runs_.TotalClusters();
}

// Dispatches to the compressed read path for a compressed attribute
// (comp_unit_size != 0), else the raw data-run walk.
template <Cache::Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadVirtualClusters(ULONGLONG vcn, ULONGLONG clusters,
                                            std::span<BYTE> buffer) const {
  assert(clusters);

  if (compression_) {
    return compression_->Read(vcn, clusters, buffer);
  }

  return ReadVirtualClustersRaw(vcn, clusters, buffer);
}

// Reads clustersToRead clusters of dataRun, starting at vcn, into the front of
// out: off the disk, or zero-filled for a sparse run. Decrypts the copy in out.
template <Cache::Strategy S>
AttrNonResident<S>::RunRead AttrNonResident<S>::ReadRunClusters(
    const Data::RunEntry& data_run, ULONGLONG vcn, ULONGLONG clusters_to_read,
    std::span<BYTE> out) const {
  if (!data_run.lcn) {
    memset(out.data(), 0, clusters_to_read * this->GetClusterSize());
    return RunRead::Done;
  }

  // Held until the copy below is done: under FullCache it owns the bytes.
  const std::optional<Cache::ReadResult<S>> bufferi =
      ReadClusters(clusters_to_read, *data_run.lcn, vcn - data_run.start_vcn);
  if (!bufferi) {
    return RunRead::ShortRead;
  }
  memcpy(out.data(), bufferi->data(), bufferi->size());

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  // Decrypts the copy in out, never the span, which may be the cache.
  if (efs_context_ && !efs_context_->Decrypt(vcn * this->GetClusterSize(),
                                             out.first(bufferi->size()))) {
    return RunRead::Failed;
  }
#endif
  return RunRead::Done;
}

// Uncompressed read path: walk the data runs, reading real clusters off disk
// and zero-filling sparse ones. Also the primitive the compressed path reads
// a unit's real (LZNT1 or stored) clusters through.
template <Cache::Strategy S>
std::optional<ULONGLONG> AttrNonResident<S>::ReadVirtualClustersRaw(
    ULONGLONG vcn, ULONGLONG clusters, std::span<BYTE> buffer) const {
  assert(clusters);

  ULONGLONG actural = 0;

  // Verify if clusters exceeds DataRun bounds
  if (vcn + clusters > TotalClusters()) {
    Log::Warn("Cluster exceeds DataRun bounds");
    return {};
  }

  // Every caller passes a buffer of exactly clusters clusters.
  assert(buffer.size() == clusters * this->GetClusterSize());

  std::span<BYTE> out = buffer;

  // Traverse the DataRun List to find the according LCN
  // Fragmented data: each pass reads the run holding the next VCN.
  while (clusters != 0) {
    const Data::RunEntry* data_run = runs_.Find(vcn);
    if (data_run == nullptr) {
      break;
    }
    // Clusters from read pointer to the end
    const ULONGLONG vcns = data_run->last_vcn - vcn + 1;
    // Fragmented data, we must go on
    const ULONGLONG clusters_to_read = clusters > vcns ? vcns : clusters;

    const RunRead status =
        ReadRunClusters(*data_run, vcn, clusters_to_read, out);
    if (status == RunRead::ShortRead) {
      break;
    }
    if (status == RunRead::Failed) {
      return {};
    }

    out = out.subspan(gsl::narrow<size_t>(
        static_cast<ULONGLONG>(clusters_to_read) * this->GetClusterSize()));
    clusters -= clusters_to_read;
    actural += clusters_to_read;
    vcn += clusters_to_read;
  }

  actural *= this->GetClusterSize();
  return actural;
}

template <Cache::Strategy S>
void AttrNonResident<S>::SetEfsContext(
    std::shared_ptr<const Efs::Context> context) noexcept {
  efs_context_ = std::move(context);
}

template <Cache::Strategy S>
const BYTE* AttrNonResident<S>::GetData() const noexcept {
  return reinterpret_cast<const BYTE*>(&attr_header_nr_);
}

// Return Actural Data Size
// *allocSize = Allocated Size
// not no except
template <Cache::Strategy S>
ULONGLONG AttrNonResident<S>::GetDataSize() const noexcept {
  return attr_header_nr_.real_size;
}

template <Cache::Strategy S>
ULONGLONG AttrNonResident<S>::GetAllocatedSize() const noexcept {
  return attr_header_nr_.alloc_size;
}

// Reads the cluster at "vcn" and copies out.size() of its bytes, from byte
// "from", into "out". An empty "out" reads nothing and succeeds.
template <Cache::Strategy S>
bool AttrNonResident<S>::ReadClusterPart(ULONGLONG vcn, DWORD from,
                                         std::span<BYTE> out) const {
  if (out.empty()) {
    return true;
  }

  const std::span<BYTE> cluster = this->volume_.GetClusterBuffer();
  std::optional<ULONGLONG> lenc = ReadVirtualClusters(vcn, 1, cluster);
  if (!lenc || *lenc != this->GetClusterSize()) {
    return false;
  }

  // Callers keep from + out.size() within one cluster, so the copy stays in
  // the cluster buffer.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const BYTE* const source = &cluster[from];
  memcpy(out.data(), source, out.size());
  return true;
}

// Copies the unaligned bytes at the start of the cluster that holds "offset".
// Returns the count copied, which is zero when "offset" is cluster aligned.
template <Cache::Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadLeadingPart(ULONGLONG offset,
                                        std::span<BYTE> out) const {
  const DWORD cluster_size = this->GetClusterSize();
  // Bytes into the first cluster; zero means the read starts on a boundary.
  const auto in_cluster = gsl::narrow<DWORD>(offset % cluster_size);
  if (in_cluster == 0) {
    return ULONGLONG{0};
  }

  const ULONGLONG len =
      std::min<ULONGLONG>(cluster_size - in_cluster, out.size());
  if (!ReadClusterPart(offset / cluster_size, in_cluster,
                       out.first(gsl::narrow<size_t>(len)))) {
    return {};
  }
  return len;
}

// Reads out.size() bytes from the start of cluster "vcn": whole clusters first,
// then the last partial cluster. An empty "out" reads nothing.
template <Cache::Strategy S>
bool AttrNonResident<S>::ReadFromClusterStart(ULONGLONG vcn,
                                              std::span<BYTE> out) const {
  const ULONGLONG cluster_size = this->GetClusterSize();
  const ULONGLONG aligned_clusters = out.size() / cluster_size;
  const ULONGLONG aligned_size = aligned_clusters * cluster_size;
  if (aligned_clusters != 0) {
    // Aligned clusters
    const std::optional<ULONGLONG> lenc = ReadVirtualClusters(
        vcn, aligned_clusters, out.first(gsl::narrow<size_t>(aligned_size)));
    if (!lenc || *lenc != aligned_size) {
      return false;
    }
  }

  // Last cluster, Unaligned. Nothing is read when no bytes remain.
  return ReadClusterPart(vcn + aligned_clusters, 0,
                         out.subspan(gsl::narrow<size_t>(aligned_size)));
}

// Read "bufLen" bytes from "offset" into "bufv", bounded by "limit" total
// bytes. Number of bytes acturally read is returned in "*actural"
template <Cache::Strategy S>
std::optional<ULONGLONG> AttrNonResident<S>::ReadDataBounded(
    ULONGLONG offset, const std::span<BYTE>& buffer, ULONGLONG limit) const {
  // Hard disks can only be accessed by sectors
  // To be simple and efficient, only implemented cluster based accessing
  // So cluster unaligned data address should be processed carefully here

  // NoCache keeps no state across calls (its raw cluster reads are redone
  // every time too): a compressed stream drops its decoded units here.
  if (compression_) {
    compression_->BeginRead();
  }

  if (buffer.empty()) {
    return ULONGLONG{0};
  }

  // Bounds check
  if (offset > limit) {
    return {};
  }
  std::span<BYTE> out = buffer;
  if (offset + buffer.size() > limit) {
    out = buffer.first(gsl::narrow<DWORD>(limit - offset));
  }

  const std::optional<ULONGLONG> head = ReadLeadingPart(offset, out);
  if (!head) {
    return {};
  }
  // The head ends on a cluster boundary, or it used up the read. Either way
  // the rest starts at the beginning of a cluster.
  const ULONGLONG rest_vcn = (offset + *head) / this->GetClusterSize();
  if (!ReadFromClusterStart(rest_vcn,
                            out.subspan(gsl::narrow<size_t>(*head)))) {
    return {};
  }
  return static_cast<ULONGLONG>(out.size());
}

// Reads are bounded by real_size, and bytes from ini_size on read as zeros:
// the clusters there hold whatever the disk held before, and are never read
// (nor decrypted). real_size and ini_size are 0 on continuation instances;
// only start_vcn == 0 sets them, and a merged attribute is this first one.
template <Cache::Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadData(ULONGLONG offset,
                                 const std::span<BYTE>& buffer) const {
  layout_->CheckClusterAllocation();
  const ULONGLONG real_size = attr_header_nr_.real_size;
  if (buffer.empty()) {
    return 0;
  }
  if (offset > real_size) {
    return {};
  }

  const ULONGLONG wanted =
      (std::min<ULONGLONG>)(buffer.size(), real_size - offset);
  const ULONGLONG init_size = (std::min)(attr_header_nr_.ini_size, real_size);
  const ULONGLONG initialized =
      (offset < init_size) ? (std::min)(wanted, init_size - offset) : 0;

  if (initialized != 0) {
    const std::optional<ULONGLONG> len = ReadDataBounded(
        offset, buffer.first(gsl::narrow<size_t>(initialized)), real_size);
    if (!len || *len != initialized) {
      return {};
    }
  }

  std::fill(buffer.begin() + gsl::narrow<std::ptrdiff_t>(initialized),
            buffer.begin() + gsl::narrow<std::ptrdiff_t>(wanted),
            static_cast<BYTE>(0));
  return wanted;
}

template <Cache::Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadExtentData(ULONGLONG offset,
                                       const std::span<BYTE>& buffer) const {
  const ULONGLONG clusters = TotalClusters();
  const DWORD cluster_size = this->GetClusterSize();

  // clusters/clusterSize are untrusted; guard the multiply against overflow.
  if (cluster_size != 0 &&
      clusters > std::numeric_limits<ULONGLONG>::max() / cluster_size) {
    Log::Error("Extent size overflows: {} clusters of {} bytes", clusters,
               cluster_size);
    return {};
  }

  return ReadDataBounded(offset, buffer, clusters * cluster_size);
}

// See NonResidentLayout::ExtentsOfRange().
template <Cache::Strategy S>
std::vector<Alloc::Extent>
    AttrNonResident<S>::ExtentsOfRange(ULONGLONG offset,
                                       ULONGLONG length) const {
  return layout_->ExtentsOfRange(offset, length);
}

// See NonResidentLayout::GetExtents().
template <Cache::Strategy S>
Alloc::StreamExtents AttrNonResident<S>::GetExtents() const {
  return layout_->GetExtents();
}

// See NonResidentLayout::GetRealRanges().
template <Cache::Strategy S>
Alloc::StreamRanges AttrNonResident<S>::GetRealRanges() const {
  return layout_->GetRealRanges();
}

// See NonResidentLayout::GetLayout().
template <Cache::Strategy S>
Alloc::StreamLayout AttrNonResident<S>::GetLayout() const {
  return layout_->GetLayout();
}

template <Cache::Strategy S>
ULONGLONG AttrNonResident<S>::GetStartVcn() const noexcept {
  return attr_header_nr_.start_vcn;
}

template <Cache::Strategy S>
ULONGLONG AttrNonResident<S>::GetLastVcn() const noexcept {
  return attr_header_nr_.last_vcn;
}

// Clusters from this instance's start VCN through its last run that maps real
// clusters. See RunList::MappedClusters().
template <Cache::Strategy S>
ULONGLONG AttrNonResident<S>::MappedClusters() const noexcept {
  return runs_.MappedClusters();
}

// Rebases other's own runs onto this instance's VCN count so far and appends
// them, so the two read as one contiguous stream.
template <Cache::Strategy S>
void AttrNonResident<S>::AppendRuns(const AttrNonResident& other) {
  runs_.Append(other.runs_);
}

template class AttrNonResident<Cache::Strategy::NoCache>;
template class AttrNonResident<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
