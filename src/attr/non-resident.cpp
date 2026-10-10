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

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep

#include "data/header-non-resident.h"
#include "data/run-entry.h"
#include "log/ntfs-common.h"
#include "ntfs-browser/win-types.h"

#if defined(NTFS_BROWSER_ENABLE_EFS_CRYPTOPP) || \
    (defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
  #include "efs/efs-context.h"
#endif
#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
  #include "lznt1/decompress.h"
#endif

namespace NtfsBrowser::Attr {

namespace {

// Max comp_unit_size exponent; keeps 2^comp_unit_size from overflowing
// before use.
constexpr WORD max_comp_unit_size_shift = 16;

// Real units are <=64KiB (16 clusters * 4KB); 1MiB caps a forged
// comp_unit_size from over-allocating.
constexpr ULONGLONG max_compression_unit_size = 1024ULL * 1024ULL;

// Clusters an attribute header spans. last_vcn is inclusive, so an empty
// attribute stores it as -1 (all ones) with start_vcn 0: that is 0 clusters,
// not a count that wraps.
constexpr ULONGLONG SpannedClusters(const Data::HeaderNonResident& header) {
  const ULONGLONG span = header.last_vcn - header.start_vcn;
  return span == std::numeric_limits<ULONGLONG>::max() ? 0 : span + 1;
}

}  // namespace

template <Cache::Strategy S>
AttrNonResident<S>::AttrNonResident(const HeaderCommon& ahc,
                                    const Io::FileRecord<S>& file_record)
    : Attr::AttrBase<S>(ahc, file_record),
      attr_header_nr_(reinterpret_cast<const Data::HeaderNonResident&>(ahc)),
      merged_clusters_(SpannedClusters(attr_header_nr_)) {
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

// Parse a single DataRun unit, and advance dataRun past it. dataRun is bounded
// to the attribute (already validated against the record buffer by
// FileRecord::ParseAttrs); data_run_offset and the run stream itself are
// attacker-controlled and otherwise unbounded.
template <Cache::Strategy S>
bool AttrNonResident<S>::PickData(std::span<const BYTE>& data_run,
                                  ULONGLONG& length, LONGLONG& lcn_offset,
                                  bool recover) noexcept {
  if (data_run.empty()) {
    return false;
  }

  union Length {
    struct {
      BYTE length_bytes : 4;
      BYTE offset_bytes : 4;
    };

    BYTE size;
  };

  const Length size{.size = data_run.front()};
  data_run = data_run.subspan(1);

  if (size.length_bytes > sizeof(ULONGLONG) ||
      size.offset_bytes > sizeof(LONGLONG)) {
    Log::Recoverable(recover, "DataRun decode error 1: 0x{:02X}", size.size);
    return false;
  }

  if (data_run.size() < static_cast<size_t>(size.length_bytes) +
                            static_cast<size_t>(size.offset_bytes)) {
    Log::Recoverable(recover,
                     "DataRun decode error: run exceeds attribute bounds");
    return false;
  }

  length = 0;
  memcpy(&length, data_run.data(), size.length_bytes);

  data_run = data_run.subspan(size.length_bytes);
  if (size.offset_bytes != 0)  // Not Sparse File
  {
    // The size check above leaves at least offsetBytes bytes in dataRun.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    if (static_cast<CHAR>(data_run[size.offset_bytes - 1U]) < 0) {
      // Negative the number read.
      lcn_offset = -1;
    } else {
      lcn_offset = 0;
    }
    memcpy(&lcn_offset, data_run.data(), size.offset_bytes);

    data_run = data_run.subspan(size.offset_bytes);
  } else {
    lcn_offset = 0;
  }

  return true;
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
  std::span<const BYTE> data_run =
      attr_header_nr_.data_run_offset < attr_bytes.size()
          ? attr_bytes.subspan(attr_header_nr_.data_run_offset)
          : std::span<const BYTE>{};
  ULONGLONG length = 0;
  LONGLONG lcn_offset = 0;
  LONGLONG lcn = 0;
  ULONGLONG vcn = 0;

  while (!data_run.empty() && data_run.front() != 0) {
    if (!PickData(data_run, length, lcn_offset, recover)) {
      // PickData() already logged which check failed.
      if (!recover) {
        throw std::runtime_error("Data run is malformed.\n");
      }
      break;
    }

    if (!AppendDataRun(length, lcn_offset, lcn, vcn, recover)) {
      break;
    }
  }
}

// Applies one decoded run (its length and LCN delta) to the running lcn and
// vcn, and appends it to the run list. Returns false on an error that recovery
// tolerates; when strict, the same error throws.
template <Cache::Strategy S>
bool AttrNonResident<S>::AppendDataRun(ULONGLONG length, LONGLONG lcn_offset,
                                       LONGLONG& lcn, ULONGLONG& vcn,
                                       bool recover) {
  // lcn is never negative here, so only a positive offset can overflow the
  // sum. It MUST be caught before the addition: signed overflow is UB.
  if (lcn_offset > 0 &&
      lcn > std::numeric_limits<LONGLONG>::max() - lcn_offset) {
    Log::Recoverable(recover, "DataRun decode error: LCN overflows");
    if (!recover) {
      throw std::runtime_error("Data run LCN overflows.\n");
    }
    return false;
  }

  lcn += lcn_offset;
  if (lcn < 0) {
    Log::Recoverable(recover, "DataRun decode error 2");
    if (!recover) {
      throw std::runtime_error("Data run LCN underflows.\n");
    }
    return false;
  }

  Log::Debug("Data length = {} clusters, LCN = {}{}", length, lcn,
             lcn_offset == 0 ? ", Sparse Data" : "");

  // Store LCN, Data size (clusters) into list
  Data::RunEntry data_run;
  data_run.lcn = (lcn_offset == 0) ? std::optional<ULONGLONG>{} : lcn;
  data_run.clusters = length;
  data_run.start_vcn = vcn;
  vcn += length;
  data_run.last_vcn = vcn - 1;

  if (data_run.last_vcn >
      (attr_header_nr_.last_vcn - attr_header_nr_.start_vcn)) {
    Log::Recoverable(recover, "DataRun decode error: VCN exceeds bound");
    if (!recover) {
      throw std::runtime_error(
          "Data run VCN exceeds the attribute's declared bound.\n");
    }
    return false;
  }

  data_run_list_.push_back(data_run);
  return true;
}

// Read clusters from disk, or sparse data
// *actural = Clusters acturally read
template <Cache::Strategy S>
std::optional<std::span<const BYTE>>
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

  std::optional<std::span<const BYTE>> buffer;
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
  return merged_clusters_;
}

// Clusters belonging to the compression unit starting at "unitFirstVcn":
// a whole unit, except for a trailing partial unit at the attribute's end.
template <Cache::Strategy S>
ULONGLONG
    AttrNonResident<S>::UnitClusters(ULONGLONG unit_first_vcn) const noexcept {
  const ULONGLONG remaining = TotalClusters() - unit_first_vcn;
  return (remaining < comp_unit_clusters_) ? remaining : comp_unit_clusters_;
}

// Counts the real (non-sparse) clusters at the start of a compression unit.
// Returns an empty optional if the unit is not fully mapped, or if a real
// run follows a hole within the unit - layouts a compression unit cannot
// legally have.
template <Cache::Strategy S>
std::optional<ULONGLONG> AttrNonResident<S>::LeadingRealClusters(
    ULONGLONG unit_first_vcn, ULONGLONG unit_clusters) const noexcept {
  const ULONGLONG unit_end = unit_first_vcn + unit_clusters;
  ULONGLONG vcn = unit_first_vcn;
  ULONGLONG real_clusters = 0;
  bool saw_hole = false;

  for (const Data::RunEntry& data_run : data_run_list_) {
    if (vcn >= unit_end) {
      break;
    }
    if (data_run.last_vcn < vcn) {
      // Entirely before the unit (or before what has been counted so far).
      continue;
    }
    if (data_run.start_vcn > vcn) {
      Log::Warn("Compression unit at VCN {} is not fully mapped",
                unit_first_vcn);
      return {};
    }

    const ULONGLONG in_run = data_run.last_vcn - vcn + 1;
    const ULONGLONG left = unit_end - vcn;
    const ULONGLONG take = (in_run < left) ? in_run : left;

    if (data_run.lcn) {
      if (saw_hole) {
        Log::Warn("Compression unit at VCN {} has real clusters after a hole",
                  unit_first_vcn);
        return {};
      }
      real_clusters += take;
    } else {
      saw_hole = true;
    }

    vcn += take;
  }

  if (vcn != unit_end) {
    // The run list ran out before the unit did.
    Log::Warn("Compression unit at VCN {} is not fully mapped", unit_first_vcn);
    return {};
  }

  return real_clusters;
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
  if (unit_first_vcn >= TotalClusters()) {
    Log::Warn("Compression unit {} exceeds DataRun bounds", unit_index);
    return nullptr;
  }

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
      LeadingRealClusters(unit_first_vcn, unit_clusters);
  if (!real_clusters_opt) {
    // LeadingRealClusters() already traced which layout it rejected.
    return nullptr;
  }
  const ULONGLONG real_clusters = *real_clusters_opt;

  if (real_clusters == 0) {
    Log::Debug("Compression unit {} is sparse", unit_index);
  } else if (real_clusters == unit_clusters) {
    // Stored unit: raw, uncompressed bytes.
    const std::optional<ULONGLONG> len =
        ReadVirtualClustersRaw(unit_first_vcn, unit_clusters, unit);
    if (!len || *len != unit_size) {
      Log::Error("Cannot read stored compression unit {}", unit_index);
      return nullptr;
    }
  } else if (!DecompressUnit(unit_index, unit_first_vcn, real_clusters, unit)) {
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

  // Same two bounds checks as the raw path.
  if (vcn + clusters > TotalClusters()) {
    Log::Warn("Cluster exceeds DataRun bounds");
    return {};
  }
  if (buffer.size() != clusters * this->GetClusterSize()) {
    Log::Warn("Invalid buffer size");
    return {};
  }

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

  std::optional<std::span<const BYTE>> bufferi =
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

  // Verify if clusters exceeds DataRun bounds
  if (buffer.size() != clusters * this->GetClusterSize()) {
    Log::Warn("Invalid buffer size");
    return {};
  }

  std::span<BYTE> out = buffer;

  // Traverse the DataRun List to find the according LCN
  for (const Data::RunEntry& data_run : data_run_list_) {
    if (vcn >= data_run.start_vcn && vcn <= data_run.last_vcn) {
      // Clusters from read pointer to the end
      const ULONGLONG vcns = data_run.last_vcn - vcn + 1;
      // Fragmented data, we must go on
      const ULONGLONG clusters_to_read = clusters > vcns ? vcns : clusters;

      const RunRead status =
          ReadRunClusters(data_run, vcn, clusters_to_read, out);
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

      if (clusters == 0) {
        break;
      }
    }
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
  // the up-to-3 partial/aligned ReadVirtualClusters() calls below share one
  // decompression per unit they overlap. FullCache keeps its units for the
  // attribute's whole lifetime instead - see comp_unit_cache_'s declaration.
  if constexpr (S == Cache::Strategy::NoCache) {
    comp_unit_cache_.clear();
  }

  ULONGLONG buf_len = buffer.size();
  std::span<BYTE> out = buffer;

  ULONGLONG actural = 0;
  if (buf_len == 0) {
    return actural;
  }

  // Bounds check
  if (offset > limit) {
    return {};
  }
  if (offset + buf_len > limit) {
    buf_len = gsl::narrow<DWORD>(limit - offset);
  }

  // First cluster Number
  ULONGLONG start_vcn = offset / this->GetClusterSize();
  // Bytes in first cluster
  const auto start_bytes = gsl::narrow<DWORD>(
      this->GetClusterSize() - (offset % this->GetClusterSize()));
  // Read first cluster
  if (start_bytes != this->GetClusterSize()) {
    ULONGLONG len = 0;
    // First cluster, Unaligned
    const std::span<BYTE> unaligned_buf_first =
        this->volume_.GetClusterBuffer();
    std::optional<ULONGLONG> lenc =
        ReadVirtualClusters(start_vcn, 1, unaligned_buf_first);
    if (!lenc || *lenc != this->GetClusterSize()) {
      return {};
    }

    len = (start_bytes < buf_len) ? start_bytes : buf_len;
    // 0 < start_bytes < GetClusterSize() here, so the index is in the cluster.
    const size_t source_offset = this->GetClusterSize() - start_bytes;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const BYTE* const source = &unaligned_buf_first[source_offset];
    memcpy(out.data(), source, len);
    out = out.subspan(gsl::narrow<size_t>(len));
    buf_len -= len;
    actural += len;
    start_vcn++;
  }
  if (buf_len == 0) {
    return actural;
  }

  const ULONGLONG aligned_clusters = buf_len / this->GetClusterSize();
  if (aligned_clusters != 0) {
    // Aligned clusters
    ULONGLONG const aligned_size = aligned_clusters * this->GetClusterSize();

    std::optional<ULONGLONG> lenc =
        ReadVirtualClusters(start_vcn, aligned_clusters,
                            out.first(gsl::narrow<size_t>(aligned_size)));
    if (!lenc || *lenc != aligned_size) {
      return {};
    }

    start_vcn += aligned_clusters;
    out = out.subspan(gsl::narrow<size_t>(aligned_size));
    buf_len %= this->GetClusterSize();
    actural += *lenc;

    if (buf_len == 0) {
      return actural;
    }
  }

  // Last cluster, Unaligned
  const std::span<BYTE> unaligned_buf_last = this->volume_.GetClusterBuffer();
  std::optional<ULONGLONG> lenc =
      ReadVirtualClusters(start_vcn, 1, unaligned_buf_last);
  if (!lenc || *lenc != this->GetClusterSize()) {
    return {};
  }

  memcpy(out.data(), unaligned_buf_last.data(), buf_len);
  actural += buf_len;

  return actural;
}

// Reads are bounded by real_size, and bytes from ini_size on read as zeros:
// the clusters there hold whatever the disk held before, and are never read
// (nor decrypted). real_size and ini_size are 0 on continuation instances;
// only start_vcn == 0 sets them, and a merged attribute is this first one.
template <Cache::Strategy S>
std::optional<ULONGLONG>
    AttrNonResident<S>::ReadData(ULONGLONG offset,
                                 const std::span<BYTE>& buffer) const {
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

template <Cache::Strategy S>
ULONGLONG AttrNonResident<S>::GetStartVcn() const noexcept {
  return attr_header_nr_.start_vcn;
}

template <Cache::Strategy S>
ULONGLONG AttrNonResident<S>::GetLastVcn() const noexcept {
  return attr_header_nr_.last_vcn;
}

// Clusters from this instance's start VCN through its last run that maps real
// clusters. A sparse tail maps nothing, and last_vcn is only a declared bound.
template <Cache::Strategy S>
ULONGLONG AttrNonResident<S>::MappedClusters() const noexcept {
  ULONGLONG mapped = 0;
  for (const Data::RunEntry& data_run : data_run_list_) {
    if (data_run.lcn.has_value()) {
      mapped = data_run.last_vcn + 1;
    }
  }
  return mapped;
}

// Rebases other's own runs onto merged_clusters_ (this instance's own VCN
// count so far) and appends them, so the two read as one contiguous stream.
template <Cache::Strategy S>
void AttrNonResident<S>::AppendRuns(const AttrNonResident& other) {
  for (Data::RunEntry data_run : other.data_run_list_) {
    data_run.start_vcn += merged_clusters_;
    data_run.last_vcn += merged_clusters_;
    data_run_list_.push_back(data_run);
  }
  merged_clusters_ += other.merged_clusters_;
}

template class AttrNonResident<Cache::Strategy::NoCache>;
template class AttrNonResident<Cache::Strategy::FullCache>;

}  // namespace NtfsBrowser::Attr
