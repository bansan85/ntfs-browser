#include "extent-map.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "../attr/non-resident.h"
#include "../log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

template <Cache::Strategy S>
ExtentMap<S>::ExtentMap(DWORD cluster_size, DWORD file_record_size) noexcept
    : cluster_size_(cluster_size), file_record_size_(file_record_size) {}

// Accepts the base extent without the usual checks against an $ATTRIBUTE_LIST
// entry, since the base record has none to match.
template <Cache::Strategy S>
void ExtentMap<S>::SetBase(const Attr::AttrBase<S>& base) {
  base_ = &base;
  // Sentinel: base extent has no $ATTRIBUTE_LIST entry to check against.
  TryAdd(base, std::numeric_limits<ULONGLONG>::max());
}

template <Cache::Strategy S>
bool ExtentMap<S>::HasBase() const noexcept {
  return base_ != nullptr;
}

// Rejects attr if named, VCN-inverted, too large for a byte offset, mismatched
// with expectedStartVcn, or overlapping; otherwise inserts it into extents_ in
// sorted order.
template <Cache::Strategy S>
void ExtentMap<S>::TryAdd(const Attr::AttrBase<S>& attr,
                          ULONGLONG expected_start_vcn) {
  if (!attr.IsUnNamed()) {
    Log::Warn("$MFT DATA continuation is named; rejecting");
    return;
  }

  const auto& non_resident = static_cast<const Attr::AttrNonResident<S>&>(attr);
  const ULONGLONG start_vcn = non_resident.GetStartVcn();
  const ULONGLONG last_vcn = non_resident.GetLastVcn();

  if (start_vcn > last_vcn) {
    Log::Warn("$MFT DATA continuation has an empty/inverted VCN range");
    return;
  }

  if (last_vcn >= std::numeric_limits<ULONGLONG>::max() / cluster_size_) {
    Log::Warn("$MFT DATA continuation's last VCN ({}) overflows a byte offset",
              last_vcn);
    return;
  }

  if (expected_start_vcn != std::numeric_limits<ULONGLONG>::max() &&
      start_vcn != expected_start_vcn) {
    Log::Warn(
        "$MFT DATA continuation's start VCN ({}) doesn't match its "
        "$ATTRIBUTE_LIST entry ({})",
        start_vcn, expected_start_vcn);
    return;
  }

  const auto insert_pos =
      std::upper_bound(extents_.begin(), extents_.end(), start_vcn,
                       [](ULONGLONG vcn, const Extent& extent) {
                         return vcn < extent.start_vcn;
                       });

  const bool overlaps_previous = insert_pos != extents_.begin() &&
                                 std::prev(insert_pos)->last_vcn >= start_vcn;
  const bool overlaps_next =
      insert_pos != extents_.end() && insert_pos->start_vcn <= last_vcn;
  if (overlaps_previous || overlaps_next) {
    Log::Warn("$MFT DATA continuation overlaps an already-accepted extent");
    return;
  }

  extents_.insert(insert_pos, Extent{start_vcn, last_vcn, &attr});
}

// True if [byteOffset, byteOffset + length) is fully mapped; no I/O.
template <Cache::Strategy S>
bool ExtentMap<S>::IsRangeMapped(ULONGLONG byte_offset,
                                 ULONGLONG length) const noexcept {
  // ParseBootSector() rejects a null cluster, and every caller passes the
  // validated, non-zero file_record_size.
  assert(cluster_size_ != 0 && length != 0);

  ULONGLONG offset = byte_offset;
  const ULONGLONG end = byte_offset + length;
  while (offset < end) {
    const Extent* extent = FindExtent(offset / cluster_size_);
    if (extent == nullptr) {
      return false;
    }
    offset = (extent->last_vcn + 1) * cluster_size_;
  }
  return true;
}

// Finds the accepted extent covering vcn, or nullptr if unresolved.
template <Cache::Strategy S>
const typename ExtentMap<S>::Extent*
    ExtentMap<S>::FindExtent(ULONGLONG vcn) const noexcept {
  const auto iterator =
      std::upper_bound(extents_.begin(), extents_.end(), vcn,
                       [](ULONGLONG value, const Extent& extent) {
                         return value < extent.start_vcn;
                       });
  if (iterator == extents_.begin()) {
    return nullptr;
  }
  const Extent& candidate = *std::prev(iterator);
  return (candidate.last_vcn >= vcn) ? &candidate : nullptr;
}

// Reads $MFT's DATA attribute at a byte offset, following extent
// boundaries transparently when it is split across extension records.
template <Cache::Strategy S>
std::optional<ULONGLONG> ExtentMap<S>::ReadData(ULONGLONG offset,
                                                std::span<BYTE> buffer) const {
  ULONGLONG total_read = 0;
  std::span<BYTE> out = buffer;
  ULONGLONG remaining = buffer.size();
  ULONGLONG current_offset = offset;

  while (remaining != 0) {
    const Extent* extent = FindExtent(current_offset / cluster_size_);
    if (extent == nullptr) {
      return {};
    }

    const auto* non_resident =
        static_cast<const Attr::AttrNonResident<S>*>(extent->attr);
    const ULONGLONG extent_start_byte = extent->start_vcn * cluster_size_;
    const ULONGLONG extent_end_byte = (extent->last_vcn + 1) * cluster_size_;
    const ULONGLONG available_in_extent = extent_end_byte - current_offset;
    const ULONGLONG to_read =
        (remaining < available_in_extent) ? remaining : available_in_extent;

    const std::optional<ULONGLONG> len =
        non_resident->ReadExtentData(current_offset - extent_start_byte,
                                     out.first(gsl::narrow<size_t>(to_read)));
    if (!len || *len != to_read) {
      return {};
    }

    out = out.subspan(gsl::narrow<size_t>(to_read));
    current_offset += to_read;
    remaining -= to_read;
    total_read += to_read;
  }

  return total_read;
}

// The count behind NtfsVolume::GetRecordsCount().
template <Cache::Strategy S>
ULONGLONG ExtentMap<S>::RecordCount() const noexcept {
  // noexcept: must not crash if called before/without checking IsVolumeOK().
  if (base_ == nullptr) {
    return 0;
  }

  // Records below USER are read from a fixed address, mapped or not.
  ULONGLONG mapped_bytes =
      static_cast<ULONGLONG>(Idx::User) * file_record_size_;
  for (const Extent& extent : extents_) {
    // MappedClusters() never exceeds last_vcn + 1, which TryAdd()
    // already checked cannot overflow a byte offset.
    const ULONGLONG extent_end =
        (extent.start_vcn +
         static_cast<const Attr::AttrNonResident<S>*>(extent.attr)
             ->MappedClusters()) *
        cluster_size_;
    mapped_bytes = (std::max)(mapped_bytes, extent_end);
  }

  return (std::min)(base_->GetDataSize(), mapped_bytes) / file_record_size_;
}

template class ExtentMap<Cache::Strategy::NoCache>;
template class ExtentMap<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
