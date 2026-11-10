#include "attr/compression-units.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "attr/run-list.h"
#include "cache/strategy.h"
#include "data/header-non-resident.h"
#include "log/ntfs-common.h"
#include "win-types.h"

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

}  // namespace

template <Cache::Strategy S>
CompressionUnits<S>::CompressionUnits(Source source)
    : header_(source.header),
      runs_(source.runs),
      cluster_size_(source.cluster_size),
      raw_reader_(std::move(source.raw_reader)) {
#ifndef NTFS_BROWSER_ENABLE_DECOMPRESSION
  // Decompression is not compiled in: reject a compressed attribute
  // outright, exactly as before compression support existed.
  throw std::runtime_error(
      "Compressed attribute rejected: decompression is not compiled "
      "in.\n");
#else
  if (header_.comp_unit_size > max_comp_unit_size_shift) {
    throw std::runtime_error("Compression unit size is out of range.\n");
  }

  comp_unit_clusters_ = 1ULL << header_.comp_unit_size;
  const ULONGLONG unit_size = comp_unit_clusters_ * cluster_size_;
  if (unit_size == 0 || unit_size > max_compression_unit_size) {
    throw std::runtime_error("Compression unit size is implausibly large.\n");
  }

  if (header_.start_vcn % comp_unit_clusters_ != 0) {
    throw std::runtime_error(
        "Compressed attribute start VCN is not compression unit "
        "aligned.\n");
  }

  Log::Debug(
      "Compressed attribute: {} clusters ({} bytes) per compression unit",
      comp_unit_clusters_, unit_size);
  Log::Debug("Compressed size = {} bytes", Data::CompressedSize(header_));
#endif
}

template <Cache::Strategy S>
ULONGLONG CompressionUnits<S>::ClustersPerUnit() const noexcept {
  return comp_unit_clusters_;
}

template <Cache::Strategy S>
void CompressionUnits<S>::BeginRead() const {
  // NoCache keeps no state across calls (its raw cluster reads are redone
  // every time too), so any decompressed unit left over from a previous read
  // is dropped here. Within a read the cache still stands, so the partial and
  // aligned cluster reads that overlap a unit share one decompression.
  // FullCache keeps its units for the attribute's whole lifetime.
  if constexpr (S == Cache::Strategy::NoCache) {
    units_.clear();
  }
}

// Clusters belonging to the compression unit starting at unit_first_vcn: a
// whole unit, except for a trailing partial unit at the attribute's end.
template <Cache::Strategy S>
ULONGLONG
    CompressionUnits<S>::UnitClusters(ULONGLONG unit_first_vcn) const noexcept {
  const ULONGLONG remaining = runs_.TotalClusters() - unit_first_vcn;
  return (remaining < comp_unit_clusters_) ? remaining : comp_unit_clusters_;
}

// Reads the real_clusters stored clusters of a compressed unit and
// LZNT1-decodes them into unit, which holds the whole unit's size. Returns
// false on a read or decompression failure.
template <Cache::Strategy S>
bool CompressionUnits<S>::DecompressUnit(ULONGLONG unit_index,
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
    compressed.assign(gsl::narrow<size_t>(real_clusters * cluster_size_), 0);
  } catch (const std::exception& e) {
    Log::Error("Cannot allocate compressed data of unit {}", unit_index);
    Log::Exception(e);
    return false;
  }

  const std::optional<ULONGLONG> len =
      raw_reader_(unit_first_vcn, real_clusters, compressed);
  if (!len || *len != compressed.size()) {
    Log::Error("Cannot read compressed compression unit {}", unit_index);
    return false;
  }

  // requiredSize: expected output length - the trailing unit may compress
  // short of unit.size(), else short output is zero-padded as if valid.
  const ULONGLONG unit_first_byte = unit_first_vcn * cluster_size_;
  ULONGLONG required_size = 0;
  if (header_.real_size > unit_first_byte) {
    const ULONGLONG left = header_.real_size - unit_first_byte;
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
bool CompressionUnits<S>::FillCompressionUnit(ULONGLONG unit_index,
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
    const ULONGLONG unit_size = unit_clusters * cluster_size_;
    const std::optional<ULONGLONG> len =
        raw_reader_(unit_first_vcn, unit_clusters, unit);
    if (!len || *len != unit_size) {
      Log::Error("Cannot read stored compression unit {}", unit_index);
      return false;
    }
    return true;
  }

  return DecompressUnit(unit_index, unit_first_vcn, real_clusters, unit);
}

// Makes one whole unit from the disk, decompressing it if needed. Gives
// nothing on an allocation, read or decompression failure.
template <Cache::Strategy S>
std::optional<std::vector<BYTE>>
    CompressionUnits<S>::BuildUnit(ULONGLONG unit_index) const {
  const ULONGLONG unit_first_vcn = unit_index * comp_unit_clusters_;
  // Read() passes only VCNs below TotalClusters(), so UnitClusters() below
  // never underflows.
  assert(unit_first_vcn < runs_.TotalClusters());

  const ULONGLONG unit_clusters = UnitClusters(unit_first_vcn);
  const ULONGLONG unit_size = unit_clusters * cluster_size_;

  // Bounded by max_compression_unit_size (constructor-validated), so this
  // can't be driven arbitrarily large.
  std::vector<BYTE> unit;
  try {
    unit.assign(gsl::narrow<size_t>(unit_size), 0);
  } catch (const std::exception& e) {
    Log::Error("Cannot allocate compression unit {}", unit_index);
    Log::Exception(e);
    return std::nullopt;
  }

  const std::optional<ULONGLONG> real_clusters =
      runs_.LeadingRealClusters(unit_first_vcn, unit_clusters);
  if (!real_clusters) {
    // LeadingRealClusters() already traced which layout it rejected.
    return std::nullopt;
  }
  if (!FillCompressionUnit(unit_index, unit_first_vcn, unit_clusters,
                           *real_clusters, unit)) {
    return std::nullopt;
  }
  return unit;
}

// Materializes one whole compression unit - decompressing it if needed - and
// returns it, or nullptr on a read/decompression failure. The returned unit
// is retained in units_, so it is never decompressed twice within one read,
// nor - under FullCache - across reads.
template <Cache::Strategy S>
const std::vector<BYTE>*
    CompressionUnits<S>::GetCompressionUnit(ULONGLONG unit_index) const {
  const auto cached = units_.find(unit_index);
  if (cached != units_.end()) {
    Log::Debug("Compression unit {} served from cache", unit_index);
    return &cached->second;
  }

  std::optional<std::vector<BYTE>> unit = BuildUnit(unit_index);
  if (!unit) {
    return nullptr;
  }

  if constexpr (S == Cache::Strategy::NoCache) {
    // Evicting here still holds "decompressed at most once per call": unit
    // indices only increase within a call.
    units_.clear();
  }

  // Guarded like other allocations here: a bad_alloc must not escape
  // ReadData() into consumer code.
  try {
    return &units_.emplace(unit_index, std::move(*unit)).first->second;
  } catch (const std::exception& e) {
    Log::Error("Cannot cache compression unit {}", unit_index);
    Log::Exception(e);
    return nullptr;
  }
}

template <Cache::Strategy S>
std::optional<ULONGLONG>
    CompressionUnits<S>::Read(ULONGLONG vcn, ULONGLONG clusters,
                              std::span<BYTE> buffer) const {
  assert(comp_unit_clusters_);

  // Same bounds check as the raw path.
  if (vcn + clusters > runs_.TotalClusters()) {
    Log::Warn("Cluster exceeds DataRun bounds");
    return {};
  }
  // Every caller passes a buffer of exactly clusters clusters.
  assert(buffer.size() == clusters * cluster_size_);

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
    const ULONGLONG unit_clusters = unit->size() / cluster_size_;
    if (offset_in_unit >= unit_clusters) {
      Log::Warn("Compression unit {} is shorter than expected", unit_index);
      break;
    }

    const ULONGLONG available = unit_clusters - offset_in_unit;
    const ULONGLONG to_copy = (clusters < available) ? clusters : available;
    const ULONGLONG bytes = to_copy * cluster_size_;

    const auto byte_count = gsl::narrow<size_t>(bytes);
    // offsetInUnit < unitClusters, so the offset lies inside the unit.
    const auto source_offset =
        gsl::narrow<size_t>(offset_in_unit * cluster_size_);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const BYTE* const source = &(*unit)[source_offset];
    memcpy(out.data(), source, byte_count);

    out = out.subspan(byte_count);
    clusters -= to_copy;
    actural += to_copy;
    vcn += to_copy;
  }

  actural *= cluster_size_;
  return actural;
}

template class CompressionUnits<Cache::Strategy::NoCache>;
template class CompressionUnits<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
