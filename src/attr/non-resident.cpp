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

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "data/header-non-resident.h"
#include "data/run-entry.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "ntfs-browser/win-types.h"

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  #include "efs/efs-context.h"
#endif
#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
  #include "lznt1/decompress.h"
#endif

namespace NTFS_BROWSER_NAMESPACE::Attr {

namespace {

// Max comp_unit_size exponent; keeps 2^comp_unit_size from overflowing
// before use.
constexpr WORD max_comp_unit_size_shift = 16;

// Real units are <=64KiB (16 clusters * 4KB); 1MiB caps a forged
// comp_unit_size from over-allocating.
constexpr ULONGLONG max_compression_unit_size = 1024ULL * 1024ULL;

// last_vcn minus start_vcn, as an unsigned count. last_vcn is inclusive, so
// an empty attribute stores it as -1 (all ones) with start_vcn 0: that is
// the maximum here. An inverted range wraps mod 2^64, as it always has, and
// is computed here without unsigned overflow.
constexpr ULONGLONG VcnSpan(const Data::HeaderNonResident& header) {
  constexpr ULONGLONG max = std::numeric_limits<ULONGLONG>::max();
  if (header.last_vcn >= header.start_vcn) {
    return header.last_vcn - header.start_vcn;
  }
  return max - (header.start_vcn - header.last_vcn) + 1;
}

// Clusters an attribute header spans. The maximum span is the empty
// attribute, so it counts as 0 clusters, not a wrapped count.
constexpr ULONGLONG SpannedClusters(const Data::HeaderNonResident& header) {
  const ULONGLONG span = VcnSpan(header);
  return span == std::numeric_limits<ULONGLONG>::max() ? 0 : span + 1;
}

}  // namespace

template <Cache::Strategy S>
AttrNonResident<S>::AttrNonResident(const HeaderCommon& ahc,
                                    const Io::FileRecord<S>& file_record)
    : Attr::AttrBase<S>(ahc, file_record),
      attr_header_nr_(reinterpret_cast<const Data::HeaderNonResident&>(ahc)),
      runs_(SpannedClusters(attr_header_nr_)),
      record_deleted_(file_record.IsDeleted()),
      record_number_(file_record.GetFileReference()) {
  // total_size already covers this field (ParseAttrs()); start_vcn must be
  // unit-aligned or units decode against the wrong window.
  if (Data::HasCompressedSizeField(attr_header_nr_)) {
#ifndef NTFS_BROWSER_ENABLE_DECOMPRESSION
    // Decompression is not compiled in: reject a compressed attribute
    // outright, exactly as before compression support existed.
    throw std::runtime_error(
        "Compressed attribute rejected: decompression is not compiled "
        "in.\n");
#else
    if (attr_header_nr_.comp_unit_size > max_comp_unit_size_shift) {
      throw std::runtime_error("Compression unit size is out of range.\n");
    }

    comp_unit_clusters_ = 1ULL << attr_header_nr_.comp_unit_size;
    const ULONGLONG unit_size = comp_unit_clusters_ * this->GetClusterSize();
    if (unit_size == 0 || unit_size > max_compression_unit_size) {
      throw std::runtime_error("Compression unit size is implausibly large.\n");
    }

    if (attr_header_nr_.start_vcn % comp_unit_clusters_ != 0) {
      throw std::runtime_error(
          "Compressed attribute start VCN is not compression unit "
          "aligned.\n");
    }

    Log::Debug(
        "Compressed attribute: {} clusters ({} bytes) per compression unit",
        comp_unit_clusters_, unit_size);
    Log::Debug("Compressed size = {} bytes",
               Data::CompressedSize(attr_header_nr_));
#endif
  }

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
  runs_.Parse(data_run, VcnSpan(attr_header_nr_), recover);
}

// Read clusters from disk, or sparse data
// *actural = Clusters acturally read
template <Cache::Strategy S>
std::optional<Cache::ReadResult<S>>
    AttrNonResident<S>::ReadClusters(ULONGLONG clusters, ULONGLONG start_lcn,
                                     ULONGLONG offset) const {
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

// Clusters belonging to the compression unit starting at "unitFirstVcn":
// a whole unit, except for a trailing partial unit at the attribute's end.
template <Cache::Strategy S>
ULONGLONG
    AttrNonResident<S>::UnitClusters(ULONGLONG unit_first_vcn) const noexcept {
  const ULONGLONG remaining = TotalClusters() - unit_first_vcn;
  return (remaining < comp_unit_clusters_) ? remaining : comp_unit_clusters_;
}

// Reads the realClusters stored clusters of a compressed unit and LZNT1-decodes
// them into unit, which holds the whole unit's size. Returns false on a read or
// decompression failure.
template <Cache::Strategy S>
bool AttrNonResident<S>::DecompressUnit(ULONGLONG unit_index,
                                        ULONGLONG unit_first_vcn,
                                        ULONGLONG real_clusters,
                                        std::vector<BYTE>& unit) const {
#ifndef NTFS_BROWSER_ENABLE_DECOMPRESSION
  // Unreachable: the constructor already rejects a compressed attribute
  // when decompression is not compiled in. Kept so this still compiles.
  Log::Error("Decompression is not compiled in.");
  return false;
#else
  std::vector<BYTE> compressed;
  try {
    compressed.assign(
        gsl::narrow<size_t>(real_clusters * this->GetClusterSize()), 0);
  } catch (const std::exception& e) {
    Log::Error("Cannot allocate compressed data of unit {}", unit_index);
    Log::Exception(e);
    return false;
  }

  const std::optional<ULONGLONG> len =
      ReadVirtualClustersRaw(unit_first_vcn, real_clusters, compressed);
  if (!len || *len != compressed.size()) {
    Log::Error("Cannot read compressed compression unit {}", unit_index);
    return false;
  }

  // requiredSize: expected output length - the trailing unit may compress
  // short of unit.size(), else short output is zero-padded as if valid.
  const ULONGLONG unit_first_byte = unit_first_vcn * this->GetClusterSize();
  ULONGLONG required_size = 0;
  if (attr_header_nr_.real_size > unit_first_byte) {
    const ULONGLONG left = attr_header_nr_.real_size - unit_first_byte;
    required_size = std::min<ULONGLONG>(left, unit.size());
  }

  try {
    const size_t produced = Lznt1::Decompress(compressed, unit);
    Log::Debug("Decompressed compression unit {} into {} bytes", unit_index,
               static_cast<ULONGLONG>(produced));
    if (produced < required_size) {
      Log::Warn(
          "Compression unit {} decompressed to {} bytes, expected at "
          "least {}",
          unit_index, static_cast<ULONGLONG>(produced), required_size);
      return false;
    }
  } catch (const std::exception& e) {
    Log::Error("Cannot decompress compression unit {}", unit_index);
    Log::Exception(e);
    return false;
  }
  return true;
#endif
}

// Fills unit, sized to the whole unit, from its real_clusters stored clusters:
// copied as is, LZNT1-decoded, or left zero when sparse. False on failure.
template <Cache::Strategy S>
bool AttrNonResident<S>::FillCompressionUnit(ULONGLONG unit_index,
                                             ULONGLONG unit_first_vcn,
                                             ULONGLONG unit_clusters,
                                             ULONGLONG real_clusters,
                                             std::vector<BYTE>& unit) const {
  if (real_clusters == 0) {
    Log::Debug("Compression unit {} is sparse", unit_index);
    return true;
  }

  if (real_clusters == unit_clusters) {
    // Stored unit: raw, uncompressed bytes.
    const ULONGLONG unit_size = unit_clusters * this->GetClusterSize();
    const std::optional<ULONGLONG> len =
        ReadVirtualClustersRaw(unit_first_vcn, unit_clusters, unit);
    if (!len || *len != unit_size) {
      Log::Error("Cannot read stored compression unit {}", unit_index);
      return false;
    }
    return true;
  }

  return DecompressUnit(unit_index, unit_first_vcn, real_clusters, unit);
}

// Materializes one whole compression unit - decompressing it if needed - and
// returns it, or nullptr on a read/decompression failure. The returned unit
// is retained in comp_unit_cache_, so it is never decompressed twice within
// one ReadData() call, nor - under FullCache - across calls.
template <Cache::Strategy S>
const std::vector<BYTE>*
    AttrNonResident<S>::GetCompressionUnit(ULONGLONG unit_index) const {
  const auto cached = comp_unit_cache_.find(unit_index);
  if (cached != comp_unit_cache_.end()) {
    Log::Debug("Compression unit {} served from cache", unit_index);
    return &cached->second;
  }

  const ULONGLONG unit_first_vcn = unit_index * comp_unit_clusters_;
  // ReadVirtualClustersCompressed() passes only VCNs below TotalClusters(),
  // so UnitClusters() below never underflows.
  assert(unit_first_vcn < TotalClusters());

  const ULONGLONG unit_clusters = UnitClusters(unit_first_vcn);
  const ULONGLONG unit_size = unit_clusters * this->GetClusterSize();

  // Bounded by max_compression_unit_size (constructor-validated), so this
  // can't be driven arbitrarily large.
  std::vector<BYTE> unit;
  try {
    unit.assign(gsl::narrow<size_t>(unit_size), 0);
  } catch (const std::exception& e) {
    Log::Error("Cannot allocate compression unit {}", unit_index);
    Log::Exception(e);
    return nullptr;
  }

  const std::optional<ULONGLONG> real_clusters_opt =
      runs_.LeadingRealClusters(unit_first_vcn, unit_clusters);
  if (!real_clusters_opt) {
    // LeadingRealClusters() already traced which layout it rejected.
    return nullptr;
  }
  if (!FillCompressionUnit(unit_index, unit_first_vcn, unit_clusters,
                           *real_clusters_opt, unit)) {
    return nullptr;
  }

  if constexpr (S == Cache::Strategy::NoCache) {
    // Evicting here still holds "decompressed at most once per call": unit
    // indices only increase within a call.
    comp_unit_cache_.clear();
  }

  // Guarded like other allocations here: a bad_alloc must not escape
  // ReadData() into consumer code.
  try {
    return &comp_unit_cache_.emplace(unit_index, std::move(unit)).first->second;
  } catch (const std::exception& e) {
    Log::Error("Cannot cache compression unit {}", unit_index);
    Log::Exception(e);
    return nullptr;
  }
}

// Compressed counterpart of ReadVirtualClustersRaw() below: serves the
// requested virtual clusters out of whole compression units instead of
// straight off the data runs.
template <Cache::Strategy S>
std::optional<ULONGLONG> AttrNonResident<S>::ReadVirtualClustersCompressed(
    ULONGLONG vcn, ULONGLONG clusters, std::span<BYTE> buffer) const {
  assert(comp_unit_clusters_);

  // Same bounds check as the raw path.
  if (vcn + clusters > TotalClusters()) {
    Log::Warn("Cluster exceeds DataRun bounds");
    return {};
  }
  // Every caller passes a buffer of exactly clusters clusters.
  assert(buffer.size() == clusters * this->GetClusterSize());

  std::span<BYTE> out = buffer;
  ULONGLONG actural = 0;

  while (clusters != 0) {
    const ULONGLONG unit_index = vcn / comp_unit_clusters_;
    const ULONGLONG unit_first_vcn = unit_index * comp_unit_clusters_;

    const std::vector<BYTE>* unit = GetCompressionUnit(unit_index);
    if (unit == nullptr) {
      break;
    }

    const ULONGLONG offset_in_unit = vcn - unit_first_vcn;
    const ULONGLONG unit_clusters = unit->size() / this->GetClusterSize();
    if (offset_in_unit >= unit_clusters) {
      Log::Warn("Compression unit {} is shorter than expected", unit_index);
      break;
    }

    const ULONGLONG available = unit_clusters - offset_in_unit;
    const ULONGLONG to_copy = (clusters < available) ? clusters : available;
    const ULONGLONG bytes = to_copy * this->GetClusterSize();

    const auto byte_count = gsl::narrow<size_t>(bytes);
    // offsetInUnit < unitClusters, so the offset lies inside the unit.
    const auto source_offset =
        gsl::narrow<size_t>(offset_in_unit * this->GetClusterSize());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const BYTE* const source = &(*unit)[source_offset];
    memcpy(out.data(), source, byte_count);

    out = out.subspan(byte_count);
    clusters -= to_copy;
    actural += to_copy;
    vcn += to_copy;
  }

  actural *= this->GetClusterSize();
  return actural;
}

// Dispatches to the compressed read path for a compressed attribute
// (comp_unit_size != 0), else the raw data-run walk.
template <Cache::Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadVirtualClusters(ULONGLONG vcn, ULONGLONG clusters,
                                            std::span<BYTE> buffer) const {
  assert(clusters);

  if (comp_unit_clusters_ != 0) {
    return ReadVirtualClustersCompressed(vcn, clusters, buffer);
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
  // every time too), so any decompressed unit left over from a previous
  // ReadData() is dropped here. Within this call the cache still stands, so
  // the up-to-3 partial/aligned ReadVirtualClusters() calls made by the
  // helpers below share one decompression per unit they overlap. FullCache
  // keeps its units for the attribute's whole lifetime instead - see
  // comp_unit_cache_'s declaration.
  if constexpr (S == Cache::Strategy::NoCache) {
    comp_unit_cache_.clear();
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

// Looks, once, at the clusters of a live data stream in the cluster bitmap and
// in the bad list. A run on a free cluster, or on a cluster marked bad, is a
// sign of damage: the volume and its metadata disagree. Each is logged apart,
// once per stream, and a cluster is in one count only. It only matters when
// recovering, so strict mode reads nothing more. The volume's own metadata
// (records below Mft::Idx::User, $Bitmap and $BadClus included) and freed
// records are left out: a freed file's clusters are free by nature, and
// checking $Bitmap against itself would loop.
template <Cache::Strategy S>
void AttrNonResident<S>::CheckClusterAllocation() const {
  if (alloc_checked_) {
    return;
  }
  alloc_checked_ = true;

  if (!this->volume_.GetOptions().recover_errors || record_deleted_ ||
      this->GetAttrType() != Attr::Type::Data || !record_number_) {
    return;
  }
  const ULONGLONG record = *record_number_ & Mft::mft_record_number_mask;
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
    this->volume_.GetRangeStates(queries);

    ULONGLONG free_clusters = 0;
    ULONGLONG bad_clusters = 0;
    std::optional<ULONGLONG> first_free_lcn;
    std::optional<ULONGLONG> first_bad_lcn;
    for (const Alloc::RangeQuery& query : queries) {
      if (query.free_clusters != 0) {
        free_clusters += query.free_clusters;
        if (!first_free_lcn) {
          first_free_lcn = query.lcn;
        }
      }
      if (query.bad_clusters != 0) {
        bad_clusters += query.bad_clusters;
        if (!first_bad_lcn) {
          first_bad_lcn = query.lcn;
        }
      }
    }
    if (first_free_lcn) {
      Log::Recoverable(true,
                       "Record {}: {} clusters of a data stream are free in "
                       "$Bitmap, from the run at LCN {}",
                       record, free_clusters, *first_free_lcn);
    }
    if (first_bad_lcn) {
      Log::Recoverable(true,
                       "Record {}: {} clusters of a data stream are marked bad "
                       "in $BadClus, from the run at LCN {}",
                       record, bad_clusters, *first_bad_lcn);
    }
  } catch (const std::exception& e) {
    Log::Exception(e);
  }
}

// Reads are bounded by real_size, and bytes from ini_size on read as zeros:
// the clusters there hold whatever the disk held before, and are never read
// (nor decrypted). real_size and ini_size are 0 on continuation instances;
// only start_vcn == 0 sets them, and a merged attribute is this first one.
template <Cache::Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadData(ULONGLONG offset,
                                 const std::span<BYTE>& buffer) const {
  CheckClusterAllocation();
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

// The runs that map clusters. The list is complete when it reaches the end of
// the allocated size the first record declares. A shorter list means a run was
// lost: recover mode stops at a bad run, and an extension record that failed to
// parse leaves its part of the stream out.
template <Cache::Strategy S>
Alloc::StreamExtents AttrNonResident<S>::GetExtents() const {
  RunList::ExtentList list = runs_.Extents();
  Alloc::StreamExtents extents{.extents = std::move(list.extents),
                               .complete = list.well_formed};

  const ULONGLONG cluster_size = this->GetClusterSize();
  if (cluster_size != 0) {
    const ULONGLONG allocated = attr_header_nr_.alloc_size;
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
Alloc::StreamRanges AttrNonResident<S>::GetRealRanges() const {
  const RunList::VcnRunList list = runs_.RealRanges();
  Alloc::StreamRanges result{.complete = list.well_formed};

  const ULONGLONG cluster_size = this->GetClusterSize();
  if (cluster_size == 0) {
    result.complete = false;
    return result;
  }

  const ULONGLONG allocated = attr_header_nr_.alloc_size;
  const ULONGLONG needed =
      allocated / cluster_size + (allocated % cluster_size != 0 ? 1 : 0);
  if (list.covered_clusters < needed) {
    result.complete = false;
  }

  const ULONGLONG data_size = GetDataSize();
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
