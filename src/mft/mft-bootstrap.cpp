#include "mft/mft-bootstrap.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr/non-resident.h"  // IWYU pragma: keep
#include "attr/slot.h"
#include "data/attribute-list.h"
#include "io/file-record-impl.h"
#include "log/ntfs-common.h"
#include "mft/extent-map.h"
#include "mft/file-reference.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// Caps the $MFT DATA entries read from a forged $ATTRIBUTE_LIST.
constexpr size_t max_mft_attr_list_entries = 65536;

}  // namespace

template <Cache::Strategy S>
MftBootstrap<S>::MftBootstrap(NtfsVolume<S>& volume)
    : volume_(&volume), record_(volume) {
  // The volume's own metadata reads always see their own content, whatever
  // include_deleted says: a freed $MFT would otherwise make the whole volume
  // unreadable.
  record_.impl_->bypass_deleted_gate = true;
}

// Parses $MFT's base record for its DATA attribute. False when it does not
// parse.
template <Cache::Strategy S>
bool MftBootstrap<S>::ParseRecord() {
  // Skips SetAttrMask()'s automatic ATTRIBUTE_LIST bit (resolved later).
  record_.impl_->attr_mask = Attr::Mask::Data;
  return record_.ParseFileRecord(static_cast<DWORD>(Idx::Mft)) &&
         record_.ParseAttrs();
}

// Finds $MFT's base DATA extent: the unnamed, non-resident instance at VCN 0.
// Null when there is none.
template <Cache::Strategy S>
const Attr::AttrBase<S>* MftBootstrap<S>::FindBaseExtent() const {
  for (const std::unique_ptr<Attr::AttrBase<S>>& attr :
       record_.GetAttr(Attr::Type::Data)) {
    if (attr->IsNonResident() && attr->IsUnNamed() &&
        static_cast<const Attr::AttrNonResident<S>*>(attr.get())
                ->GetStartVcn() == 0) {
      return attr.get();
    }
  }
  return nullptr;
}

// Warns when $MFT's declared size covers more records than its extents map. The
// record count then stops at the mapped ones.
template <Cache::Strategy S>
void MftBootstrap<S>::WarnIfClaimsMoreThanMapped(
    const Attr::AttrBase<S>& base_extent) const {
  if (map_.RecordCount() < base_extent.GetDataSize() / file_record_size_) {
    Log::Warn(
        "$MFT claims {} bytes but maps fewer; counting {} records instead of "
        "{}",
        base_extent.GetDataSize(), map_.RecordCount(),
        base_extent.GetDataSize() / file_record_size_);
  }
}

template <Cache::Strategy S>
void MftBootstrap<S>::MapExtents(const Attr::AttrBase<S>& base_extent,
                                 DWORD cluster_size, DWORD file_record_size) {
  file_record_size_ = file_record_size;
  map_ = ExtentMap<S>(cluster_size, file_record_size);
  map_.SetBase(base_extent);

  // Must run after map_ has its base extent, so it can use the map.
  ResolveMftDataExtents();

  WarnIfClaimsMoreThanMapped(base_extent);
}

template <Cache::Strategy S>
const ExtentMap<S>& MftBootstrap<S>::Map() const noexcept {
  return map_;
}

// Resolves $MFT's own DATA continuations named by its $ATTRIBUTE_LIST, as a
// fixed point since one entry can depend on an extent only another reveals.
template <Cache::Strategy S>
void MftBootstrap<S>::ResolveMftDataExtents() {
  // Isolated from record_; resolve_attr_list_ = false skips AttrList.
  Io::FileRecord<S> list_record(*volume_);
  list_record.impl_->attr_mask = Attr::Mask::AttributeList;
  list_record.impl_->resolve_attr_list = false;
  list_record.impl_->bypass_deleted_gate = true;

  if (!list_record.ParseFileRecord(static_cast<DWORD>(Idx::Mft)) ||
      !list_record.ParseAttrs()) {
    Log::Debug("$MFT's own $ATTRIBUTE_LIST did not parse; assuming none");
    return;
  }

  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& list_attrs =
      list_record.GetAttr(Attr::Type::AttributeList);
  if (list_attrs.empty()) {
    return;  // $MFT's DATA attribute fits in the base record alone.
  }
  const Attr::AttrBase<S>& raw_list = *list_attrs.front();
  // Set whenever ParseFileRecord() above succeeded, so never empty here.
  const std::optional<ULONGLONG> list_ref = list_record.GetFileReference();
  assert(list_ref.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const ULONGLONG self_ref = *list_ref;

  std::vector<PendingMftExtension> pending =
      CollectPendingMftExtensions(raw_list, self_ref);

  // Attempts each ref once reachable, dropping it either way to bound reads.
  while (!pending.empty()) {
    std::vector<PendingMftExtension> still_pending;
    bool attempted_any = false;

    for (PendingMftExtension& item : pending) {
      const bool reachable = item.record < static_cast<ULONGLONG>(Idx::User) ||
                             map_.IsRangeMapped(item.record * file_record_size_,
                                                file_record_size_);
      if (!reachable) {
        still_pending.push_back(std::move(item));
        continue;
      }
      attempted_any = true;
      ResolvePendingMftExtension(item, self_ref);
    }

    if (!attempted_any) {
      break;
    }
    pending = std::move(still_pending);
  }

  if (!pending.empty()) {
    Log::Warn(
        "{} of $MFT's own DATA continuation(s) could not be resolved "
        "(unreachable)",
        pending.size());
  }
}

// Collects each DATA entry of rawList, grouped per extension record, and
// capped. One record can hold several extents, so it keeps every start VCN
// listed.
template <Cache::Strategy S>
std::vector<typename MftBootstrap<S>::PendingMftExtension>
    MftBootstrap<S>::CollectPendingMftExtensions(
        const Attr::AttrBase<S>& raw_list, ULONGLONG self_ref) {
  std::vector<PendingMftExtension> pending;
  std::unordered_map<ULONGLONG, size_t> index_by_ref;
  size_t listed_entries = 0;
  ULONGLONG offset = 0;
  Data::AttributeList entry{};
  while (listed_entries < max_mft_attr_list_entries) {
    const std::optional<ULONGLONG> len =
        raw_list.ReadData(offset, {reinterpret_cast<BYTE*>(&entry),
                                   Data::attribute_list_entry_header_size});
    if (!len) {
      break;
    }
    if (*len != Data::attribute_list_entry_header_size ||
        !Attr::IsValidAttrType(entry.attr_type)) {
      break;
    }

    const ULONGLONG record_ref = entry.base_ref.segment_number;
    if (entry.attr_type == Attr::Type::Data && entry.name_length == 0 &&
        record_ref != self_ref) {
      listed_entries++;
      // A file reference packs the record number and its sequence number.
      const ULONGLONG key =
          record_ref | (static_cast<ULONGLONG>(entry.base_ref.sequence_number)
                        << mft_sequence_shift);
      const auto [iterator, inserted] =
          index_by_ref.emplace(key, pending.size());
      if (inserted) {
        pending.push_back({record_ref,
                           static_cast<WORD>(entry.base_ref.sequence_number),
                           {}});
      }
      // it->second is the index of an entry of pending, pushed above or
      // earlier.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      pending[iterator->second].start_vcns.push_back(entry.start_vcn);
    }

    if (entry.record_size == 0) {
      break;
    }
    offset += entry.record_size;
  }
  return pending;
}

// Opens item's extension record and hands each of its DATA extents to
// map_.TryAdd(). The record is dropped again if it is not usable.
template <Cache::Strategy S>
void MftBootstrap<S>::ResolvePendingMftExtension(
    const PendingMftExtension& item, ULONGLONG self_ref) {
  extension_records_.emplace_back(*volume_);
  Io::FileRecord<S>& ext = extension_records_.back();
  ext.impl_->attr_mask = Attr::Mask::Data;
  ext.impl_->bypass_deleted_gate = true;

  const bool parsed = ext.ParseFileRecord(item.record);
  // A record another file reused since the list was written is not
  // $MFT's extension: its $DATA would map foreign clusters.
  if (parsed &&
      !IsGenuineExtensionRecord(item.sequence, ext.GetSequenceNumber(),
                                ext.GetBaseRecordReference(),
                                self_ref & mft_record_number_mask)) {
    extension_records_.pop_back();
    Log::Warn(
        "$MFT DATA continuation in record {} is not an extension of "
        "$MFT (reused or foreign); ignoring",
        item.record);
    return;
  }
  if (!parsed || !ext.ParseAttrs()) {
    extension_records_.pop_back();
    Log::Warn("$MFT DATA continuation in record {} could not be resolved",
              item.record);
    return;
  }

  for (const std::unique_ptr<Attr::AttrBase<S>>& attr :
       ext.GetAttr(Attr::Type::Data)) {
    if (attr->IsNonResident()) {
      // Any start VCN not listed is rejected by map_.TryAdd().
      const ULONGLONG start_vcn =
          static_cast<const Attr::AttrNonResident<S>&>(*attr).GetStartVcn();
      const auto listed = std::ranges::find(item.start_vcns, start_vcn);
      map_.TryAdd(*attr, listed != item.start_vcns.end()
                             ? *listed
                             : item.start_vcns.front());
    }
  }
}

template class MftBootstrap<Cache::Strategy::NoCache>;
template class MftBootstrap<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
