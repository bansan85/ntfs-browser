#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "attr-non-resident.h"  // IWYU pragma: keep
#include "attr-resident.h"
#include "attr-slot.h"
#include "attr-vol-info.h"
#include "attr-vol-name.h"
#include "data/attribute-list.h"
#include "data/file-record-header.h"
#include "data/index-block.h"
#include "data/ntfs-bpb.h"
#include "file-reader.h"  // IWYU pragma: keep
#include "file-record-impl.h"
#include "mft-file-reference.h"
#include "ntfs-common.h"
#include "ntfs-volume-impl.h"
#include "upcase.h"
#include "utf.h"

namespace NtfsBrowser {

class IDiskReader;

namespace Efs {

class IEfsKeyProvider;

}  // namespace Efs

namespace Attr {

struct HeaderCommon;

}  // namespace Attr
struct VolumeOptions;
template <Cache::Strategy S>
class AttrBase;

namespace {

// AttrVolName pads its buffer with a terminator that its view still
// covers. UTF-8 has no terminator convention, so the padding must go
// before converting, or it becomes a NUL byte inside the log line.
std::wstring_view TrimTrailingNuls(std::wstring_view name) noexcept {
  while (!name.empty() && name.back() == L'\0') {
    name.remove_suffix(1);
  }
  return name;
}

// Length of a Win32 volume device path, NUL excluded: \\.\C:
constexpr size_t volume_path_length = 6;

// Largest -log2 of sectors per cluster the BPB encoding may carry: a cluster
// of 2^12 sectors, far beyond any real volume.
constexpr int max_sectors_per_cluster_shift = 12;

// Largest -log2 of a file record or index block size, in bytes.
constexpr int max_size_shift = 12;

// Largest literal cluster count of a file record or index block.
constexpr int max_size_in_clusters = 8;

// Caps the $MFT DATA entries read from a forged $ATTRIBUTE_LIST.
constexpr size_t max_mft_attr_list_entries = 65536;

// Decodes a BPB size byte: a positive value counts clusters, a negative one is
// -log2 of the size in bytes. Rejects a magnitude that would shift 1U by 32 or
// more (undefined behaviour), or yield a size no real volume could have.
std::optional<DWORD> DecodeBpbSize(char raw, DWORD cluster_size) {
  if (raw < -max_size_shift || raw > max_size_in_clusters) {
    return std::nullopt;
  }
  if (raw > 0) {
    return cluster_size * raw;
  }
  return 1U << static_cast<unsigned char>(-raw);
}

// Computes the cluster size from the sectors-per-cluster BPB byte, or nullopt
// when its magnitude is out of range.
std::optional<DWORD> DecodeClusterSize(char spc, WORD sector_size) {
  if (spc >= 0) {
    return sector_size * static_cast<unsigned char>(spc);
  }
  // Windows 10 1903+ (build 18362) large-cluster encoding: a negative byte
  // is -log2(sectors per cluster), not a literal sector count, letting a
  // single BYTE field reach cluster sizes above 255 sectors (up to 2 MiB
  // at the common 512-byte sector size).
  if (spc < -max_sectors_per_cluster_shift) {
    return std::nullopt;
  }
  return sector_size * (1U << static_cast<unsigned char>(-spc));
}

}  // namespace

template <Cache::Strategy S>
NtfsVolume<S>::Impl::Impl(NtfsVolume<S>& self, const VolumeOptions& options)
    : self(&self),
      volume(std::make_unique<Io::FileReader<S>>()),
      mft_record(self),
      options(options) {}

#ifdef _WIN32
template <Cache::Strategy S>
NtfsVolume<S>::NtfsVolume(_TCHAR volume, const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options)) {
  if (impl_->OpenVolume(volume)) {
    impl_->Init();
  }
}

template <Cache::Strategy S>
NtfsVolume<S>::NtfsVolume(std::wstring_view path, const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options)) {
  if (impl_->OpenVolume(path)) {
    impl_->Init();
  }
}
#endif

template <Cache::Strategy S>
NtfsVolume<S>::NtfsVolume(std::unique_ptr<IDiskReader> reader,
                          const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options)) {
  if (impl_->OpenVolume(std::move(reader))) {
    impl_->Init();
  }
}

template <Cache::Strategy S>
NtfsVolume<S>::~NtfsVolume() = default;

// Verify NTFS volume version (must >= 3.0) and locate $MFT's Data attribute
template <Cache::Strategy S>
void NtfsVolume<S>::Impl::Init() {
  // The volume's own metadata reads always see their own content, whatever
  // include_deleted says: they are not the caller's traversal of the
  // filesystem, and a freed $Volume/$MFT would otherwise make the whole
  // volume unreadable.
  mft_record.impl_->bypass_deleted_gate = true;

  FileRecord vol(*self);
  vol.impl_->bypass_deleted_gate = true;
  vol.SetAttrMask(Attr::Mask::VolumeName | Attr::Mask::VolumeInformation);
  if (!vol.ParseFileRecord(static_cast<DWORD>(Mft::Idx::Volume))) {
    return;
  }

  if (!vol.ParseAttrs()) {
    return;
  }
  const auto& vec = vol.GetAttr(Attr::Type::VolumeInformation);
  if (vec.empty()) {
    return;
  }

  if constexpr (S == Cache::Strategy::NoCache) {
    std::tie(version_major, version_minor) =
        reinterpret_cast<const Attr::AttrVolInfo<Attr::AttrResidentNoCache,
                                                 Cache::Strategy::NoCache>*>(
            vec.front().get())
            ->GetVersion();
  } else {
    std::tie(version_major, version_minor) =
        reinterpret_cast<const Attr::AttrVolInfo<Attr::AttrResidentFullCache,
                                                 Cache::Strategy::FullCache>*>(
            vec.front().get())
            ->GetVersion();
  }
  Log::Info("NTFS volume version: {}.{}", version_major, version_minor);
  if (version_major < 3)  // NT4 ?
  {
    return;
  }

  const auto& vec2 = vol.GetAttr(Attr::Type::VolumeName);
  if (!vec2.empty()) {
    if constexpr (S == Cache::Strategy::NoCache) {
      const std::wstring_view volname =
          reinterpret_cast<const Attr::AttrVolName<Attr::AttrResidentNoCache,
                                                   Cache::Strategy::NoCache>*>(
              vec2.front().get())
              ->GetName();
      Log::Info("NTFS volume name: {}",
                Utf::WideToUtf8(TrimTrailingNuls(volname)));
    } else {
      const std::wstring_view volname =
          reinterpret_cast<const Attr::AttrVolName<
              Attr::AttrResidentFullCache, Cache::Strategy::FullCache>*>(
              vec2.front().get())
              ->GetName();
      Log::Info("NTFS volume name: {}",
                Utf::WideToUtf8(TrimTrailingNuls(volname)));
    }
  }

  // Skips SetAttrMask()'s automatic ATTRIBUTE_LIST bit (resolved below).
  mft_record.impl_->attr_mask = Attr::Mask::Data;
  if (!mft_record.ParseFileRecord(static_cast<DWORD>(Mft::Idx::Mft)) ||
      !mft_record.ParseAttrs()) {
    return;
  }

  const AttrBase<S>* base_extent = nullptr;
  for (const std::unique_ptr<AttrBase<S>>& attr :
       mft_record.GetAttr(Attr::Type::Data)) {
    // The base extent is the unnamed, non-resident DATA instance at VCN 0.
    if (attr->IsNonResident() && attr->IsUnNamed() &&
        static_cast<const Attr::AttrNonResident<S>*>(attr.get())
                ->GetStartVcn() == 0) {
      base_extent = attr.get();
      break;
    }
  }
  if (base_extent == nullptr) {
    return;
  }

  mft_data = base_extent;

  // Sentinel: base extent has no $ATTRIBUTE_LIST entry to check against.
  TryAddMftExtent(*base_extent, std::numeric_limits<ULONGLONG>::max());

  // Must run after mft_data_/mft_extents_ are set, so it can use them.
  ResolveMftDataExtents();

  if (GetRecordsCount() < base_extent->GetDataSize() / file_record_size) {
    Log::Warn(
        "$MFT claims {} bytes but maps fewer; counting {} records instead of "
        "{}",
        base_extent->GetDataSize(), GetRecordsCount(),
        base_extent->GetDataSize() / file_record_size);
  }

  // Reported OK only once mft_data_ is actually assigned.
  volume_ok = true;
}

// Resolves $MFT's own DATA continuations named by its $ATTRIBUTE_LIST, as a
// fixed point since one entry can depend on an extent only another reveals.
template <Cache::Strategy S>
void NtfsVolume<S>::Impl::ResolveMftDataExtents() {
  // Isolated from mft_record_; resolve_attr_list_ = false skips AttrList.
  FileRecord<S> list_record(*self);
  list_record.impl_->attr_mask = Attr::Mask::AttributeList;
  list_record.impl_->resolve_attr_list = false;
  list_record.impl_->bypass_deleted_gate = true;

  if (!list_record.ParseFileRecord(static_cast<DWORD>(Mft::Idx::Mft)) ||
      !list_record.ParseAttrs()) {
    Log::Debug("$MFT's own $ATTRIBUTE_LIST did not parse; assuming none");
    return;
  }

  const std::vector<std::unique_ptr<AttrBase<S>>>& list_attrs =
      list_record.GetAttr(Attr::Type::AttributeList);
  if (list_attrs.empty()) {
    return;  // $MFT's DATA attribute fits in the base record alone.
  }
  const AttrBase<S>& raw_list = *list_attrs.front();
  const std::optional<ULONGLONG> list_ref = list_record.GetFileReference();
  if (!list_ref.has_value()) {
    Log::Debug(
        "$MFT's own $ATTRIBUTE_LIST has no file reference; assuming none");
    return;
  }
  const ULONGLONG self_ref = *list_ref;

  std::vector<PendingMftExtension> pending =
      CollectPendingMftExtensions(raw_list, self_ref);

  // Attempts each ref once reachable, dropping it either way to bound reads.
  while (!pending.empty()) {
    std::vector<PendingMftExtension> still_pending;
    bool attempted_any = false;

    for (PendingMftExtension& item : pending) {
      const bool reachable =
          item.record < static_cast<ULONGLONG>(Mft::Idx::User) ||
          IsMftRangeMapped(item.record * file_record_size, file_record_size);
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
std::vector<typename NtfsVolume<S>::Impl::PendingMftExtension>
    NtfsVolume<S>::Impl::CollectPendingMftExtensions(
        const AttrBase<S>& raw_list, ULONGLONG self_ref) {
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
                        << Mft::mft_sequence_shift);
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
// TryAddMftExtent(). The record is dropped again if it is not usable.
template <Cache::Strategy S>
void NtfsVolume<S>::Impl::ResolvePendingMftExtension(
    const PendingMftExtension& item, ULONGLONG self_ref) {
  mft_extension_records.emplace_back(*self);
  FileRecord<S>& ext = mft_extension_records.back();
  ext.impl_->attr_mask = Attr::Mask::Data;
  ext.impl_->bypass_deleted_gate = true;

  const bool parsed = ext.ParseFileRecord(item.record);
  // A record another file reused since the list was written is not
  // $MFT's extension: its $DATA would map foreign clusters.
  if (parsed &&
      !Mft::IsGenuineExtensionRecord(item.sequence, ext.GetSequenceNumber(),
                                     ext.GetBaseRecordReference(),
                                     self_ref & Mft::mft_record_number_mask)) {
    mft_extension_records.pop_back();
    Log::Warn(
        "$MFT DATA continuation in record {} is not an extension of "
        "$MFT (reused or foreign); ignoring",
        item.record);
    return;
  }
  if (!parsed || !ext.ParseAttrs()) {
    mft_extension_records.pop_back();
    Log::Warn("$MFT DATA continuation in record {} could not be resolved",
              item.record);
    return;
  }

  for (const std::unique_ptr<AttrBase<S>>& attr :
       ext.GetAttr(Attr::Type::Data)) {
    if (attr->IsNonResident()) {
      // Any start VCN not listed is rejected by TryAddMftExtent().
      const ULONGLONG start_vcn =
          static_cast<const Attr::AttrNonResident<S>&>(*attr).GetStartVcn();
      const auto listed = std::ranges::find(item.start_vcns, start_vcn);
      TryAddMftExtent(*attr, listed != item.start_vcns.end()
                                 ? *listed
                                 : item.start_vcns.front());
    }
  }
}

// Rejects attr if named, VCN-inverted, too large for a byte offset, mismatched
// with expectedStartVcn, or overlapping; otherwise inserts it into mft_extents_
// in sorted order.
template <Cache::Strategy S>
void NtfsVolume<S>::Impl::TryAddMftExtent(const AttrBase<S>& attr,
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

  if (last_vcn >= std::numeric_limits<ULONGLONG>::max() / cluster_size) {
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
      std::upper_bound(mft_extents.begin(), mft_extents.end(), start_vcn,
                       [](ULONGLONG vcn, const MftExtent& extent) {
                         return vcn < extent.start_vcn;
                       });

  const bool overlaps_previous = insert_pos != mft_extents.begin() &&
                                 std::prev(insert_pos)->last_vcn >= start_vcn;
  const bool overlaps_next =
      insert_pos != mft_extents.end() && insert_pos->start_vcn <= last_vcn;
  if (overlaps_previous || overlaps_next) {
    Log::Warn("$MFT DATA continuation overlaps an already-accepted extent");
    return;
  }

  mft_extents.insert(insert_pos, MftExtent{start_vcn, last_vcn, &attr});
}

// True if [byteOffset, byteOffset + length) is fully mapped; no I/O.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::IsMftRangeMapped(ULONGLONG byte_offset,
                                           ULONGLONG length) const noexcept {
  if (cluster_size == 0 || length == 0) {
    return false;
  }

  ULONGLONG offset = byte_offset;
  const ULONGLONG end = byte_offset + length;
  while (offset < end) {
    const MftExtent* extent = FindMftExtent(offset / cluster_size);
    if (extent == nullptr) {
      return false;
    }
    offset = (extent->last_vcn + 1) * cluster_size;
  }
  return true;
}

// Finds the accepted extent covering vcn, or nullptr if unresolved.
template <Cache::Strategy S>
const NtfsVolume<S>::Impl::MftExtent*
    NtfsVolume<S>::Impl::FindMftExtent(ULONGLONG vcn) const noexcept {
  const auto iterator =
      std::upper_bound(mft_extents.begin(), mft_extents.end(), vcn,
                       [](ULONGLONG value, const MftExtent& extent) {
                         return value < extent.start_vcn;
                       });
  if (iterator == mft_extents.begin()) {
    return nullptr;
  }
  const MftExtent& candidate = *std::prev(iterator);
  return (candidate.last_vcn >= vcn) ? &candidate : nullptr;
}

// Reads $MFT's DATA attribute at a byte offset, following extent
// boundaries transparently when it is split across extension records.
template <Cache::Strategy S>
std::optional<ULONGLONG>
    NtfsVolume<S>::Impl::ReadMftData(ULONGLONG offset,
                                     std::span<BYTE> buffer) const {
  ULONGLONG total_read = 0;
  std::span<BYTE> out = buffer;
  ULONGLONG remaining = buffer.size();
  ULONGLONG current_offset = offset;

  while (remaining != 0) {
    const MftExtent* extent = FindMftExtent(current_offset / cluster_size);
    if (extent == nullptr) {
      return {};
    }

    const auto* non_resident =
        static_cast<const Attr::AttrNonResident<S>*>(extent->attr);
    const ULONGLONG extent_start_byte = extent->start_vcn * cluster_size;
    const ULONGLONG extent_end_byte = (extent->last_vcn + 1) * cluster_size;
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

#ifdef _WIN32
// Open a volume ('a' - 'z', 'A' - 'Z'), get volume handle and BPB
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(_TCHAR volume) {
  // Verify parameter
  if (!_istalpha(volume)) {
    Log::Error("Volume name error, should be like 'C', 'D'");
    return false;
  }

  std::array<_TCHAR, volume_path_length + 1> volume_path;
  _sntprintf_s(volume_path.data(), volume_path.size(), volume_path_length,
               _T("\\\\.\\%c:"), volume);
  std::get<volume_path_length>(volume_path) = _T('\0');

  return OpenVolume(std::wstring_view(volume_path.data()));
}

// Open an arbitrary device/image path, get volume handle and BPB
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(std::wstring_view path) {
  if (!volume->Open(path)) {
    Log::Error("Cannnot open volume");
    return false;
  }

  return ParseBootSector();
}
#endif

// Use an already-open reader (eg. a test double), get BPB
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(std::unique_ptr<IDiskReader> reader) {
  volume = std::make_unique<Io::FileReader<S>>(std::move(reader));

  return ParseBootSector();
}

// Read the first sector (boot sector) and derive volume geometry from it
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::ParseBootSector() {
  // Smallest sector size NTFS uses. It is also the size of the BPB read.
  constexpr DWORD min_sector_size = 512;
  // Largest sector size NTFS supports (4Kn). Windows opens a volume handle
  // unbuffered, and then only accepts a read length that is a whole number
  // of the device's sectors.
  constexpr DWORD max_sector_size = 4096;
  LARGE_INTEGER fr_addr{.QuadPart = 0};
  std::optional<std::span<const BYTE>> bpb_buffer =
      volume->Read(fr_addr, max_sector_size);
  if (!bpb_buffer) {
    // A backing file shorter than max_sector_size cannot serve that read.
    Log::Warn("Cannot read a {}-byte boot sector, retrying with {} bytes",
              max_sector_size, min_sector_size);
    fr_addr.QuadPart = 0;
    bpb_buffer = volume->Read(fr_addr, min_sector_size);
  }
  if (!bpb_buffer) {
    Log::Error("Read boot sector error");
    return false;
  }
  const auto* bpb = reinterpret_cast<const Data::NtfsBpb*>(bpb_buffer->data());

  const std::string_view signature(
      reinterpret_cast<const char*>(&bpb->signature[0]),
      sizeof(bpb->signature));
  if (signature != Data::ntfs_signature) {
    Log::Warn("Volume file system is not NTFS");
    return false;
  }

  // Log important volume parameters

  sector_size = bpb->bytes_per_sector;
  Log::Info("Sector Size = {} bytes", sector_size);

  // A sector smaller than one WORD cannot be a real BPB value.
  if (sector_size < sizeof(WORD)) {
    Log::Error("Sector Size must be at least 2 bytes");
    return false;
  }

  const std::optional<DWORD> decoded_cluster_size = DecodeClusterSize(
      static_cast<char>(bpb->sectors_per_cluster), sector_size);
  if (!decoded_cluster_size) {
    Log::Error("sectors_per_cluster magnitude out of range");
    return false;
  }
  cluster_size = *decoded_cluster_size;
  Log::Info("Cluster Size = {} bytes", cluster_size);

  if (cluster_size == 0) {
    Log::Error("Cluster Size can't be null");
    return false;
  }
  cluster_buffer.resize(cluster_size);

  const std::optional<DWORD> decoded_record_size = DecodeBpbSize(
      static_cast<char>(bpb->clusters_per_file_record), cluster_size);
  if (!decoded_record_size) {
    Log::Error("clusters_per_file_record magnitude out of range");
    return false;
  }
  file_record_size = *decoded_record_size;
  Log::Info("FileRecord Size = {} bytes", file_record_size);

  // Rejects a size too small for the header, or not a whole number of
  // sectors.
  if (file_record_size < Data::FileRecordHeader::min_file_record_header_size ||
      file_record_size % sector_size != 0) {
    Log::Error("FileRecord Size is invalid");
    return false;
  }

  if (file_record_size > Data::FileRecordHeader::max_file_record_size) {
    Log::Error(
        "FileRecord Size exceeds the maximum supported file record size");
    return false;
  }

  const std::optional<DWORD> decoded_index_block_size = DecodeBpbSize(
      static_cast<char>(bpb->clusters_per_index_block), cluster_size);
  if (!decoded_index_block_size) {
    Log::Error("clusters_per_index_block magnitude out of range");
    return false;
  }
  index_block_size = *decoded_index_block_size;
  Log::Info("IndexBlock Size = {} bytes", index_block_size);

  // Rejects a size too small for the header, or not a whole number of
  // sectors.
  if (index_block_size < sizeof(Data::IndexBlock) ||
      index_block_size % sector_size != 0) {
    Log::Error("IndexBlock Size is invalid");
    return false;
  }

  // Multiplying two attacker-controlled values can overflow mft_addr_'s type.
  const bool mft_addr_overflows =
      cluster_size != 0 &&
      bpb->lcn_mft > std::numeric_limits<ULONGLONG>::max() / cluster_size;
  mft_addr = mft_addr_overflows ? std::numeric_limits<ULONGLONG>::max()
                                : bpb->lcn_mft * cluster_size;
  Log::Info("MFT address = 0x{:016X}", mft_addr);

  // Leaves headroom for the per-record byte offset added to mft_addr_
  // later, before it is narrowed to a LONGLONG.
  constexpr ULONGLONG max_plausible_mft_addr =
      std::numeric_limits<LONGLONG>::max() / 2;

  if (mft_addr_overflows || mft_addr > max_plausible_mft_addr) {
    Log::Error("MFT address is invalid");
    return false;
  }

  return true;
}

// Check if Volume is successfully opened
template <Cache::Strategy S>
bool NtfsVolume<S>::IsVolumeOK() const noexcept {
  return impl_->volume_ok;
}

template <Cache::Strategy S>
const VolumeOptions& NtfsVolume<S>::GetOptions() const noexcept {
  return impl_->options;
}

// Get NTFS volume version
template <Cache::Strategy S>
std::pair<BYTE, BYTE> NtfsVolume<S>::GetVersion() const noexcept {
  return {impl_->version_major, impl_->version_minor};
}

// Get File Record count
template <Cache::Strategy S>
ULONGLONG NtfsVolume<S>::GetRecordsCount() const noexcept {
  return impl_->GetRecordsCount();
}

// The count behind NtfsVolume::GetRecordsCount().
template <Cache::Strategy S>
ULONGLONG NtfsVolume<S>::Impl::GetRecordsCount() const noexcept {
  // noexcept: must not crash if called before/without checking IsVolumeOK().
  if (mft_data == nullptr) {
    return 0;
  }

  // Records below USER are read from a fixed address, mapped or not.
  ULONGLONG mapped_bytes =
      static_cast<ULONGLONG>(Mft::Idx::User) * file_record_size;
  for (const MftExtent& extent : mft_extents) {
    // MappedClusters() never exceeds last_vcn + 1, which TryAddMftExtent()
    // already checked cannot overflow a byte offset.
    const ULONGLONG extent_end =
        (extent.start_vcn +
         static_cast<const Attr::AttrNonResident<S>*>(extent.attr)
             ->MappedClusters()) *
        cluster_size;
    mapped_bytes = (std::max)(mapped_bytes, extent_end);
  }

  return (std::min)(mft_data->GetDataSize(), mapped_bytes) / file_record_size;
}

// Get BPB information
template <Cache::Strategy S>
WORD NtfsVolume<S>::GetSectorSize() const noexcept {
  return impl_->sector_size;
}

template <Cache::Strategy S>
DWORD NtfsVolume<S>::GetClusterSize() const noexcept {
  return impl_->cluster_size;
}

template <Cache::Strategy S>
DWORD NtfsVolume<S>::GetFileRecordSize() const noexcept {
  return impl_->file_record_size;
}

template <Cache::Strategy S>
DWORD NtfsVolume<S>::GetIndexBlockSize() const noexcept {
  return impl_->index_block_size;
}

// Get MFT starting address
template <Cache::Strategy S>
ULONGLONG NtfsVolume<S>::GetMFTAddr() const noexcept {
  return impl_->mft_addr;
}

template <Cache::Strategy S>
std::span<BYTE> NtfsVolume<S>::GetClusterBuffer() const noexcept {
  return {impl_->cluster_buffer.data(), impl_->cluster_buffer.size()};
}

template <Cache::Strategy S>
std::optional<std::span<const BYTE>> NtfsVolume<S>::Read(LARGE_INTEGER& addr,
                                                         DWORD length) const {
  return impl_->volume->Read(addr, length);
}

template <Cache::Strategy S>
bool NtfsVolume<S>::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const {
  return impl_->volume->ReadInto(addr, dest);
}

// Install Attribute CallBack routines for the whole Volume
template <Cache::Strategy S>
bool NtfsVolume<S>::InstallAttrRawCB(Attr::Type attr_type,
                                     Attr::RawCallback callback) noexcept {
  const DWORD at_idx = Attr::AttrIndex(attr_type);
  if (at_idx >= Attr::attr_nums) {
    return false;
  }

  // atIdx < attr_nums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  impl_->attr_raw_call_back[at_idx] = callback;
  return true;
}

// attType is an already-bounds-checked index into attr_raw_call_back_
// (attr_nums), not a raw Attr::Type/DWORD value. Only FileRecord<S>, a friend,
// calls it, from an index it already validated, which avoids exposing an
// unbounded array index through the public API. See N11 in
// docs/bug-reports/2026-09-03-full-repo.md.
template <Cache::Strategy S>
void NtfsVolume<S>::Impl::AttrRawCallBack(DWORD att_type,
                                          const Attr::HeaderCommon& ahc,
                                          bool& discard) const {
  // The caller passes an index below attr_nums, as described above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  if (attr_raw_call_back[att_type] != nullptr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    attr_raw_call_back[att_type](ahc, discard);
  }
}

// Clear all Attribute CallBack routines
template <Cache::Strategy S>
void NtfsVolume<S>::ClearAttrRawCB() noexcept {
  for (Attr::RawCallback& call_back : impl_->attr_raw_call_back) {
    call_back = nullptr;
  }
}

template <Cache::Strategy S>
void NtfsVolume<S>::SetEfsKeyProvider(
    std::shared_ptr<Efs::IEfsKeyProvider> provider) noexcept {
  impl_->efs_provider = std::move(provider);
  impl_->efs_provider_set = true;
}

template <Cache::Strategy S>
std::shared_ptr<Efs::IEfsKeyProvider> NtfsVolume<S>::GetEfsKeyProvider() const {
  if (!impl_->efs_provider_set) {
    impl_->efs_provider_set = true;
#ifdef _WIN32
    impl_->efs_provider = Efs::MakeCertStoreKeyProvider();
#endif
  }
  return impl_->efs_provider;
}

template <Cache::Strategy S>
bool NtfsVolume<S>::SetEfsCipherBackend(Efs::CipherBackend backend) noexcept {
  if (backend == Efs::CipherBackend::CryptoPp) {
#ifndef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
    return false;
#endif
  } else if (backend == Efs::CipherBackend::BCrypt) {
#if !(defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
    return false;
#endif
  }
  impl_->efs_backend = backend;
  return true;
}

template <Cache::Strategy S>
Efs::CipherBackend NtfsVolume<S>::GetEfsCipherBackend() const noexcept {
  return impl_->efs_backend;
}

// Reads $UpCase (MFT record 10). Null when it is missing or unusable.
template <Cache::Strategy S>
std::unique_ptr<const UpCase::Table>
    NtfsVolume<S>::Impl::LoadUpCaseTable() const {
  const auto upcase_record = static_cast<ULONGLONG>(Mft::Idx::UpCase);
  if (!volume_ok ||
      !IsMftRangeMapped(upcase_record * file_record_size, file_record_size)) {
    return {};
  }

  // Like the volume's other metadata reads, it MUST NOT depend on
  // include_deleted.
  FileRecord<S> record(*self);
  record.impl_->bypass_deleted_gate = true;
  record.SetAttrMask(Attr::Mask::Data);
  if (!record.ParseFileRecord(upcase_record) || !record.ParseAttrs()) {
    return {};
  }

  const AttrBase<S>* data = record.FindStream({});
  if (data == nullptr || data->GetDataSize() < UpCase::Table::byte_count) {
    return {};
  }

  std::vector<BYTE> bytes(UpCase::Table::byte_count);
  const std::optional<ULONGLONG> len = data->ReadData(0, bytes);
  if (!len || *len != bytes.size()) {
    return {};
  }

  std::optional<UpCase::Table> table = UpCase::Table::FromBytes(bytes);
  if (!table) {
    return {};
  }
  return std::make_unique<const UpCase::Table>(std::move(*table));
}

// Loads $UpCase on first use. A failure is cached: the built-in mapping
// answers every later call.
template <Cache::Strategy S>
const UpCase::Table& NtfsVolume<S>::Impl::GetUpCaseTable() const {
  if (!upcase_loaded) {
    upcase_loaded = true;
    upcase = LoadUpCaseTable();
    if (!upcase) {
      Log::Info("$UpCase is not usable: names collate by the built-in mapping");
    }
  }
  return upcase ? *upcase : UpCase::Table::BuiltIn();
}

template class NtfsVolume<Cache::Strategy::NoCache>;
template class NtfsVolume<Cache::Strategy::FullCache>;

}  // namespace NtfsBrowser
