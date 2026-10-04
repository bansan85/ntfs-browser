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
#include "attr/attribute-list.h"
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

namespace NtfsBrowser
{
class IDiskReader;

namespace Efs
{
class IEfsKeyProvider;
}  // namespace Efs
struct AttrHeaderCommon;
struct VolumeOptions;
template <Strategy S>
class AttrBase;

namespace
{

// AttrVolName pads its buffer with a terminator that its view still
// covers. UTF-8 has no terminator convention, so the padding must go
// before converting, or it becomes a NUL byte inside the log line.
std::wstring_view TrimTrailingNuls(std::wstring_view name) noexcept
{
  while (!name.empty() && name.back() == L'\0')
  {
    name.remove_suffix(1);
  }
  return name;
}

// Length of a Win32 volume device path, NUL excluded: \\.\C:
constexpr size_t kVolumePathLength = 6;

// Largest -log2 of sectors per cluster the BPB encoding may carry: a cluster
// of 2^12 sectors, far beyond any real volume.
constexpr int kMaxSectorsPerClusterShift = 12;

// Largest -log2 of a file record or index block size, in bytes.
constexpr int kMaxSizeShift = 12;

// Largest literal cluster count of a file record or index block.
constexpr int kMaxSizeInClusters = 8;

// Caps the $MFT DATA entries read from a forged $ATTRIBUTE_LIST.
constexpr size_t kMaxMftAttrListEntries = 65536;

// Decodes a BPB size byte: a positive value counts clusters, a negative one is
// -log2 of the size in bytes. Rejects a magnitude that would shift 1U by 32 or
// more (undefined behaviour), or yield a size no real volume could have.
std::optional<DWORD> DecodeBpbSize(char raw, DWORD clusterSize)
{
  if (raw < -kMaxSizeShift || raw > kMaxSizeInClusters)
  {
    return std::nullopt;
  }
  if (raw > 0)
  {
    return clusterSize * raw;
  }
  return 1U << static_cast<unsigned char>(-raw);
}

// Computes the cluster size from the sectors-per-cluster BPB byte, or nullopt
// when its magnitude is out of range.
std::optional<DWORD> DecodeClusterSize(char spc, WORD sectorSize)
{
  if (spc >= 0)
  {
    return sectorSize * static_cast<unsigned char>(spc);
  }
  // Windows 10 1903+ (build 18362) large-cluster encoding: a negative byte
  // is -log2(sectors per cluster), not a literal sector count, letting a
  // single BYTE field reach cluster sizes above 255 sectors (up to 2 MiB
  // at the common 512-byte sector size).
  if (spc < -kMaxSectorsPerClusterShift)
  {
    return std::nullopt;
  }
  return sectorSize * (1U << static_cast<unsigned char>(-spc));
}

}  // namespace

template <Strategy S>
NtfsVolume<S>::Impl::Impl(NtfsVolume<S>& self, const VolumeOptions& options)
    : self_(&self),
      volume_(std::make_unique<FileReader<S>>()),
      mft_record_(self),
      options_(options)
{
}

#ifdef _WIN32
template <Strategy S>
NtfsVolume<S>::NtfsVolume(_TCHAR volume, const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options))
{
  if (impl_->OpenVolume(volume))
  {
    impl_->Init();
  }
}

template <Strategy S>
NtfsVolume<S>::NtfsVolume(std::wstring_view path, const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options))
{
  if (impl_->OpenVolume(path))
  {
    impl_->Init();
  }
}
#endif

template <Strategy S>
NtfsVolume<S>::NtfsVolume(std::unique_ptr<IDiskReader> reader,
                          const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options))
{
  if (impl_->OpenVolume(std::move(reader)))
  {
    impl_->Init();
  }
}

template <Strategy S>
NtfsVolume<S>::~NtfsVolume() = default;

// Verify NTFS volume version (must >= 3.0) and locate $MFT's Data attribute
template <Strategy S>
void NtfsVolume<S>::Impl::Init()
{
  // The volume's own metadata reads always see their own content, whatever
  // include_deleted says: they are not the caller's traversal of the
  // filesystem, and a freed $Volume/$MFT would otherwise make the whole
  // volume unreadable.
  mft_record_.impl_->bypass_deleted_gate_ = true;

  FileRecord vol(*self_);
  vol.impl_->bypass_deleted_gate_ = true;
  vol.SetAttrMask(Mask::VOLUME_NAME | Mask::VOLUME_INFORMATION);
  if (!vol.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::VOLUME)))
  {
    return;
  }

  if (!vol.ParseAttrs())
  {
    return;
  }
  const auto& vec = vol.getAttr(AttrType::VOLUME_INFORMATION);
  if (vec.empty())
  {
    return;
  }

  if constexpr (S == Strategy::NO_CACHE)
  {
    std::tie(version_major_, version_minor_) =
        reinterpret_cast<
            const AttrVolInfo<AttrResidentNoCache, Strategy::NO_CACHE>*>(
            vec.front().get())
            ->GetVersion();
  }
  else
  {
    std::tie(version_major_, version_minor_) =
        reinterpret_cast<
            const AttrVolInfo<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
            vec.front().get())
            ->GetVersion();
  }
  LogInfo("NTFS volume version: {}.{}", version_major_, version_minor_);
  if (version_major_ < 3)  // NT4 ?
  {
    return;
  }

  const auto& vec2 = vol.getAttr(AttrType::VOLUME_NAME);
  if (!vec2.empty())
  {
    if constexpr (S == Strategy::NO_CACHE)
    {
      const std::wstring_view volname =
          reinterpret_cast<
              const AttrVolName<AttrResidentNoCache, Strategy::NO_CACHE>*>(
              vec2.front().get())
              ->GetName();
      LogInfo("NTFS volume name: {}", WideToUtf8(TrimTrailingNuls(volname)));
    }
    else
    {
      const std::wstring_view volname =
          reinterpret_cast<
              const AttrVolName<AttrResidentFullCache, Strategy::FULL_CACHE>*>(
              vec2.front().get())
              ->GetName();
      LogInfo("NTFS volume name: {}", WideToUtf8(TrimTrailingNuls(volname)));
    }
  }

  // Skips SetAttrMask()'s automatic ATTRIBUTE_LIST bit (resolved below).
  mft_record_.impl_->attr_mask_ = Mask::DATA;
  if (!mft_record_.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::MFT)) ||
      !mft_record_.ParseAttrs())
  {
    return;
  }

  const AttrBase<S>* baseExtent = nullptr;
  for (const std::unique_ptr<AttrBase<S>>& attr :
       mft_record_.getAttr(AttrType::DATA))
  {
    // The base extent is the unnamed, non-resident DATA instance at VCN 0.
    if (attr->IsNonResident() && attr->IsUnNamed() &&
        static_cast<const AttrNonResident<S>*>(attr.get())->GetStartVcn() == 0)
    {
      baseExtent = attr.get();
      break;
    }
  }
  if (baseExtent == nullptr)
  {
    return;
  }

  mft_data_ = baseExtent;

  // Sentinel: base extent has no $ATTRIBUTE_LIST entry to check against.
  TryAddMftExtent(*baseExtent, std::numeric_limits<ULONGLONG>::max());

  // Must run after mft_data_/mft_extents_ are set, so it can use them.
  ResolveMftDataExtents();

  if (GetRecordsCount() < baseExtent->GetDataSize() / file_record_size_)
  {
    LogWarn(
        "$MFT claims {} bytes but maps fewer; counting {} records instead of "
        "{}",
        baseExtent->GetDataSize(), GetRecordsCount(),
        baseExtent->GetDataSize() / file_record_size_);
  }

  // Reported OK only once mft_data_ is actually assigned.
  volume_ok_ = true;
}

// Resolves $MFT's own DATA continuations named by its $ATTRIBUTE_LIST, as a
// fixed point since one entry can depend on an extent only another reveals.
template <Strategy S>
void NtfsVolume<S>::Impl::ResolveMftDataExtents()
{
  // Isolated from mft_record_; resolve_attr_list_ = false skips AttrList.
  FileRecord<S> listRecord(*self_);
  listRecord.impl_->attr_mask_ = Mask::ATTRIBUTE_LIST;
  listRecord.impl_->resolve_attr_list_ = false;
  listRecord.impl_->bypass_deleted_gate_ = true;

  if (!listRecord.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::MFT)) ||
      !listRecord.ParseAttrs())
  {
    LogDebug("$MFT's own $ATTRIBUTE_LIST did not parse; assuming none");
    return;
  }

  const std::vector<std::unique_ptr<AttrBase<S>>>& listAttrs =
      listRecord.getAttr(AttrType::ATTRIBUTE_LIST);
  if (listAttrs.empty())
  {
    return;  // $MFT's DATA attribute fits in the base record alone.
  }
  const AttrBase<S>& rawList = *listAttrs.front();
  const std::optional<ULONGLONG> listRef = listRecord.GetFileReference();
  if (!listRef.has_value())
  {
    LogDebug("$MFT's own $ATTRIBUTE_LIST has no file reference; assuming none");
    return;
  }
  const ULONGLONG selfRef = *listRef;

  std::vector<PendingMftExtension> pending =
      CollectPendingMftExtensions(rawList, selfRef);

  // Attempts each ref once reachable, dropping it either way to bound reads.
  while (!pending.empty())
  {
    std::vector<PendingMftExtension> stillPending;
    bool attemptedAny = false;

    for (PendingMftExtension& item : pending)
    {
      const bool reachable =
          item.record < static_cast<ULONGLONG>(Enum::MftIdx::USER) ||
          IsMftRangeMapped(item.record * file_record_size_, file_record_size_);
      if (!reachable)
      {
        stillPending.push_back(std::move(item));
        continue;
      }
      attemptedAny = true;
      ResolvePendingMftExtension(item, selfRef);
    }

    if (!attemptedAny)
    {
      break;
    }
    pending = std::move(stillPending);
  }

  if (!pending.empty())
  {
    LogWarn(
        "{} of $MFT's own DATA continuation(s) could not be resolved "
        "(unreachable)",
        pending.size());
  }
}

// Collects each DATA entry of rawList, grouped per extension record, and capped.
// One record can hold several extents, so it keeps every start VCN listed.
template <Strategy S>
std::vector<typename NtfsVolume<S>::Impl::PendingMftExtension>
    NtfsVolume<S>::Impl::CollectPendingMftExtensions(const AttrBase<S>& rawList,
                                                     ULONGLONG selfRef)
{
  std::vector<PendingMftExtension> pending;
  std::unordered_map<ULONGLONG, size_t> indexByRef;
  size_t listedEntries = 0;
  ULONGLONG offset = 0;
  Attr::AttributeList entry{};
  while (listedEntries < kMaxMftAttrListEntries)
  {
    const std::optional<ULONGLONG> len =
        rawList.ReadData(offset, {reinterpret_cast<BYTE*>(&entry),
                                  Attr::kAttributeListEntryHeaderSize});
    if (!len)
    {
      break;
    }
    if (*len != Attr::kAttributeListEntryHeaderSize ||
        !IsValidAttrType(entry.attr_type))
    {
      break;
    }

    const ULONGLONG recordRef = entry.base_ref.segment_number;
    if (entry.attr_type == AttrType::DATA && entry.name_length == 0 &&
        recordRef != selfRef)
    {
      listedEntries++;
      // A file reference packs the record number and its sequence number.
      const ULONGLONG key =
          recordRef | (static_cast<ULONGLONG>(entry.base_ref.sequence_number)
                       << kMftSequenceShift);
      const auto [iterator, inserted] = indexByRef.emplace(key, pending.size());
      if (inserted)
      {
        pending.push_back(
            {recordRef, static_cast<WORD>(entry.base_ref.sequence_number), {}});
      }
      // it->second is the index of an entry of pending, pushed above or earlier.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      pending[iterator->second].start_vcns.push_back(entry.start_vcn);
    }

    if (entry.record_size == 0)
    {
      break;
    }
    offset += entry.record_size;
  }
  return pending;
}

// Opens item's extension record and hands each of its DATA extents to
// TryAddMftExtent(). The record is dropped again if it is not usable.
template <Strategy S>
void NtfsVolume<S>::Impl::ResolvePendingMftExtension(
    const PendingMftExtension& item, ULONGLONG selfRef)
{
  mft_extension_records_.emplace_back(*self_);
  FileRecord<S>& ext = mft_extension_records_.back();
  ext.impl_->attr_mask_ = Mask::DATA;
  ext.impl_->bypass_deleted_gate_ = true;

  const bool parsed = ext.ParseFileRecord(item.record);
  // A record another file reused since the list was written is not
  // $MFT's extension: its $DATA would map foreign clusters.
  if (parsed &&
      !IsGenuineExtensionRecord(item.sequence, ext.GetSequenceNumber(),
                                ext.GetBaseRecordReference(),
                                selfRef & kMftRecordNumberMask))
  {
    mft_extension_records_.pop_back();
    LogWarn(
        "$MFT DATA continuation in record {} is not an extension of "
        "$MFT (reused or foreign); ignoring",
        item.record);
    return;
  }
  if (!parsed || !ext.ParseAttrs())
  {
    mft_extension_records_.pop_back();
    LogWarn("$MFT DATA continuation in record {} could not be resolved",
            item.record);
    return;
  }

  for (const std::unique_ptr<AttrBase<S>>& attr : ext.getAttr(AttrType::DATA))
  {
    if (attr->IsNonResident())
    {
      // Any start VCN not listed is rejected by TryAddMftExtent().
      const ULONGLONG startVcn =
          static_cast<const AttrNonResident<S>&>(*attr).GetStartVcn();
      const auto listed = std::ranges::find(item.start_vcns, startVcn);
      TryAddMftExtent(*attr, listed != item.start_vcns.end()
                                 ? *listed
                                 : item.start_vcns.front());
    }
  }
}

// Rejects attr if named, VCN-inverted, too large for a byte offset, mismatched
// with expectedStartVcn, or overlapping; otherwise inserts it into mft_extents_
// in sorted order.
template <Strategy S>
void NtfsVolume<S>::Impl::TryAddMftExtent(const AttrBase<S>& attr,
                                          ULONGLONG expectedStartVcn)
{
  if (!attr.IsUnNamed())
  {
    LogWarn("$MFT DATA continuation is named; rejecting");
    return;
  }

  const auto& nonResident = static_cast<const AttrNonResident<S>&>(attr);
  const ULONGLONG startVcn = nonResident.GetStartVcn();
  const ULONGLONG lastVcn = nonResident.GetLastVcn();

  if (startVcn > lastVcn)
  {
    LogWarn("$MFT DATA continuation has an empty/inverted VCN range");
    return;
  }

  if (lastVcn >= std::numeric_limits<ULONGLONG>::max() / cluster_size_)
  {
    LogWarn("$MFT DATA continuation's last VCN ({}) overflows a byte offset",
            lastVcn);
    return;
  }

  if (expectedStartVcn != std::numeric_limits<ULONGLONG>::max() &&
      startVcn != expectedStartVcn)
  {
    LogWarn(
        "$MFT DATA continuation's start VCN ({}) doesn't match its "
        "$ATTRIBUTE_LIST entry ({})",
        startVcn, expectedStartVcn);
    return;
  }

  const auto insertPos =
      std::upper_bound(mft_extents_.begin(), mft_extents_.end(), startVcn,
                       [](ULONGLONG vcn, const MftExtent& extent)
                       { return vcn < extent.start_vcn; });

  const bool overlapsPrevious = insertPos != mft_extents_.begin() &&
                                std::prev(insertPos)->last_vcn >= startVcn;
  const bool overlapsNext =
      insertPos != mft_extents_.end() && insertPos->start_vcn <= lastVcn;
  if (overlapsPrevious || overlapsNext)
  {
    LogWarn("$MFT DATA continuation overlaps an already-accepted extent");
    return;
  }

  mft_extents_.insert(insertPos, MftExtent{startVcn, lastVcn, &attr});
}

// True if [byteOffset, byteOffset + length) is fully mapped; no I/O.
template <Strategy S>
bool NtfsVolume<S>::Impl::IsMftRangeMapped(ULONGLONG byteOffset,
                                           ULONGLONG length) const noexcept
{
  if (cluster_size_ == 0 || length == 0)
  {
    return false;
  }

  ULONGLONG offset = byteOffset;
  const ULONGLONG end = byteOffset + length;
  while (offset < end)
  {
    const MftExtent* extent = FindMftExtent(offset / cluster_size_);
    if (extent == nullptr)
    {
      return false;
    }
    offset = (extent->last_vcn + 1) * cluster_size_;
  }
  return true;
}

// Finds the accepted extent covering vcn, or nullptr if unresolved.
template <Strategy S>
const NtfsVolume<S>::Impl::MftExtent*
    NtfsVolume<S>::Impl::FindMftExtent(ULONGLONG vcn) const noexcept
{
  const auto iterator =
      std::upper_bound(mft_extents_.begin(), mft_extents_.end(), vcn,
                       [](ULONGLONG value, const MftExtent& extent)
                       { return value < extent.start_vcn; });
  if (iterator == mft_extents_.begin())
  {
    return nullptr;
  }
  const MftExtent& candidate = *std::prev(iterator);
  return (candidate.last_vcn >= vcn) ? &candidate : nullptr;
}

// Reads $MFT's DATA attribute at a byte offset, following extent
// boundaries transparently when it is split across extension records.
template <Strategy S>
std::optional<ULONGLONG>
    NtfsVolume<S>::Impl::ReadMftData(ULONGLONG offset,
                                     std::span<BYTE> buffer) const
{
  ULONGLONG totalRead = 0;
  std::span<BYTE> out = buffer;
  ULONGLONG remaining = buffer.size();
  ULONGLONG currentOffset = offset;

  while (remaining != 0)
  {
    const MftExtent* extent = FindMftExtent(currentOffset / cluster_size_);
    if (extent == nullptr)
    {
      return {};
    }

    const auto* nonResident =
        static_cast<const AttrNonResident<S>*>(extent->attr);
    const ULONGLONG extentStartByte = extent->start_vcn * cluster_size_;
    const ULONGLONG extentEndByte = (extent->last_vcn + 1) * cluster_size_;
    const ULONGLONG availableInExtent = extentEndByte - currentOffset;
    const ULONGLONG toRead =
        (remaining < availableInExtent) ? remaining : availableInExtent;

    const std::optional<ULONGLONG> len =
        nonResident->ReadExtentData(currentOffset - extentStartByte,
                                    out.first(gsl::narrow<size_t>(toRead)));
    if (!len || *len != toRead)
    {
      return {};
    }

    out = out.subspan(gsl::narrow<size_t>(toRead));
    currentOffset += toRead;
    remaining -= toRead;
    totalRead += toRead;
  }

  return totalRead;
}

#ifdef _WIN32
// Open a volume ('a' - 'z', 'A' - 'Z'), get volume handle and BPB
template <Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(_TCHAR volume)
{
  // Verify parameter
  if (!_istalpha(volume))
  {
    LogError("Volume name error, should be like 'C', 'D'");
    return false;
  }

  std::array<_TCHAR, kVolumePathLength + 1> volumePath;
  _sntprintf_s(volumePath.data(), volumePath.size(), kVolumePathLength,
               _T("\\\\.\\%c:"), volume);
  std::get<kVolumePathLength>(volumePath) = _T('\0');

  return OpenVolume(std::wstring_view(volumePath.data()));
}

// Open an arbitrary device/image path, get volume handle and BPB
template <Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(std::wstring_view path)
{
  if (!volume_->Open(path))
  {
    LogError("Cannnot open volume");
    return false;
  }

  return ParseBootSector();
}
#endif

// Use an already-open reader (eg. a test double), get BPB
template <Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(std::unique_ptr<IDiskReader> reader)
{
  volume_ = std::make_unique<FileReader<S>>(std::move(reader));

  return ParseBootSector();
}

// Read the first sector (boot sector) and derive volume geometry from it
template <Strategy S>
bool NtfsVolume<S>::Impl::ParseBootSector()
{
  // Smallest sector size NTFS uses. It is also the size of the BPB read.
  constexpr DWORD kMinSectorSize = 512;
  // Largest sector size NTFS supports (4Kn). Windows opens a volume handle
  // unbuffered, and then only accepts a read length that is a whole number
  // of the device's sectors.
  constexpr DWORD kMaxSectorSize = 4096;
  LARGE_INTEGER frAddr{.QuadPart = 0};
  std::optional<std::span<const BYTE>> bpb_buffer =
      volume_->Read(frAddr, kMaxSectorSize);
  if (!bpb_buffer)
  {
    // A backing file shorter than kMaxSectorSize cannot serve that read.
    LogWarn("Cannot read a {}-byte boot sector, retrying with {} bytes",
            kMaxSectorSize, kMinSectorSize);
    frAddr.QuadPart = 0;
    bpb_buffer = volume_->Read(frAddr, kMinSectorSize);
  }
  if (!bpb_buffer)
  {
    LogError("Read boot sector error");
    return false;
  }
  const auto* bpb = reinterpret_cast<const Data::NtfsBpb*>(bpb_buffer->data());

  const std::string_view signature(
      reinterpret_cast<const char*>(&bpb->signature[0]),
      sizeof(bpb->signature));
  if (signature != Data::kNtfsSignature)
  {
    LogWarn("Volume file system is not NTFS");
    return false;
  }

  // Log important volume parameters

  sector_size_ = bpb->bytes_per_sector;
  LogInfo("Sector Size = {} bytes", sector_size_);

  // A sector smaller than one WORD cannot be a real BPB value.
  if (sector_size_ < sizeof(WORD))
  {
    LogError("Sector Size must be at least 2 bytes");
    return false;
  }

  const std::optional<DWORD> clusterSize = DecodeClusterSize(
      static_cast<char>(bpb->sectors_per_cluster), sector_size_);
  if (!clusterSize)
  {
    LogError("sectors_per_cluster magnitude out of range");
    return false;
  }
  cluster_size_ = *clusterSize;
  LogInfo("Cluster Size = {} bytes", cluster_size_);

  if (cluster_size_ == 0)
  {
    LogError("Cluster Size can't be null");
    return false;
  }
  cluster_buffer_.resize(cluster_size_);

  const std::optional<DWORD> recordSize = DecodeBpbSize(
      static_cast<char>(bpb->clusters_per_file_record), cluster_size_);
  if (!recordSize)
  {
    LogError("clusters_per_file_record magnitude out of range");
    return false;
  }
  file_record_size_ = *recordSize;
  LogInfo("FileRecord Size = {} bytes", file_record_size_);

  // Rejects a size too small for the header, or not a whole number of
  // sectors.
  if (file_record_size_ < kMinFileRecordHeaderSize ||
      file_record_size_ % sector_size_ != 0)
  {
    LogError("FileRecord Size is invalid");
    return false;
  }

  if (file_record_size_ > kMaxFileRecordSize)
  {
    LogError("FileRecord Size exceeds the maximum supported file record size");
    return false;
  }

  const std::optional<DWORD> indexBlockSize = DecodeBpbSize(
      static_cast<char>(bpb->clusters_per_index_block), cluster_size_);
  if (!indexBlockSize)
  {
    LogError("clusters_per_index_block magnitude out of range");
    return false;
  }
  index_block_size_ = *indexBlockSize;
  LogInfo("IndexBlock Size = {} bytes", index_block_size_);

  // Rejects a size too small for the header, or not a whole number of
  // sectors.
  if (index_block_size_ < sizeof(Data::IndexBlock) ||
      index_block_size_ % sector_size_ != 0)
  {
    LogError("IndexBlock Size is invalid");
    return false;
  }

  // Multiplying two attacker-controlled values can overflow mft_addr_'s type.
  const bool mft_addr_overflows =
      cluster_size_ != 0 &&
      bpb->lcn_mft > std::numeric_limits<ULONGLONG>::max() / cluster_size_;
  mft_addr_ = mft_addr_overflows ? std::numeric_limits<ULONGLONG>::max()
                                 : bpb->lcn_mft * cluster_size_;
  LogInfo("MFT address = 0x{:016X}", mft_addr_);

  // Leaves headroom for the per-record byte offset added to mft_addr_
  // later, before it is narrowed to a LONGLONG.
  constexpr ULONGLONG kMaxPlausibleMftAddr =
      std::numeric_limits<LONGLONG>::max() / 2;

  if (mft_addr_overflows || mft_addr_ > kMaxPlausibleMftAddr)
  {
    LogError("MFT address is invalid");
    return false;
  }

  return true;
}

// Check if Volume is successfully opened
template <Strategy S>
bool NtfsVolume<S>::IsVolumeOK() const noexcept
{
  return impl_->volume_ok_;
}

template <Strategy S>
const VolumeOptions& NtfsVolume<S>::GetOptions() const noexcept
{
  return impl_->options_;
}

// Get NTFS volume version
template <Strategy S>
std::pair<BYTE, BYTE> NtfsVolume<S>::GetVersion() const noexcept
{
  return {impl_->version_major_, impl_->version_minor_};
}

// Get File Record count
template <Strategy S>
ULONGLONG NtfsVolume<S>::GetRecordsCount() const noexcept
{
  return impl_->GetRecordsCount();
}

// The count behind NtfsVolume::GetRecordsCount().
template <Strategy S>
ULONGLONG NtfsVolume<S>::Impl::GetRecordsCount() const noexcept
{
  // noexcept: must not crash if called before/without checking IsVolumeOK().
  if (mft_data_ == nullptr)
  {
    return 0;
  }

  // Records below USER are read from a fixed address, mapped or not.
  ULONGLONG mappedBytes =
      static_cast<ULONGLONG>(Enum::MftIdx::USER) * file_record_size_;
  for (const MftExtent& extent : mft_extents_)
  {
    // MappedClusters() never exceeds last_vcn + 1, which TryAddMftExtent()
    // already checked cannot overflow a byte offset.
    const ULONGLONG extentEnd =
        (extent.start_vcn + static_cast<const AttrNonResident<S>*>(extent.attr)
                                ->MappedClusters()) *
        cluster_size_;
    mappedBytes = (std::max)(mappedBytes, extentEnd);
  }

  return (std::min)(mft_data_->GetDataSize(), mappedBytes) / file_record_size_;
}

// Get BPB information
template <Strategy S>
WORD NtfsVolume<S>::GetSectorSize() const noexcept
{
  return impl_->sector_size_;
}

template <Strategy S>
DWORD NtfsVolume<S>::GetClusterSize() const noexcept
{
  return impl_->cluster_size_;
}

template <Strategy S>
DWORD NtfsVolume<S>::GetFileRecordSize() const noexcept
{
  return impl_->file_record_size_;
}

template <Strategy S>
DWORD NtfsVolume<S>::GetIndexBlockSize() const noexcept
{
  return impl_->index_block_size_;
}

// Get MFT starting address
template <Strategy S>
ULONGLONG NtfsVolume<S>::GetMFTAddr() const noexcept
{
  return impl_->mft_addr_;
}

template <Strategy S>
std::span<BYTE> NtfsVolume<S>::GetClusterBuffer() const noexcept
{
  return {impl_->cluster_buffer_.data(), impl_->cluster_buffer_.size()};
}

template <Strategy S>
std::optional<std::span<const BYTE>> NtfsVolume<S>::Read(LARGE_INTEGER& addr,
                                                         DWORD length) const
{
  return impl_->volume_->Read(addr, length);
}

template <Strategy S>
bool NtfsVolume<S>::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const
{
  return impl_->volume_->ReadInto(addr, dest);
}

// Install Attribute CallBack routines for the whole Volume
template <Strategy S>
bool NtfsVolume<S>::InstallAttrRawCB(AttrType attrType,
                                     AttrRawCallback callback) noexcept
{
  const DWORD atIdx = AttrIndex(attrType);
  if (atIdx >= kAttrNums)
  {
    return false;
  }

  // atIdx < kAttrNums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  impl_->attr_raw_call_back_[atIdx] = callback;
  return true;
}

// attType is an already-bounds-checked index into attr_raw_call_back_
// (kAttrNums), not a raw AttrType/DWORD value. Only FileRecord<S>, a friend,
// calls it, from an index it already validated, which avoids exposing an
// unbounded array index through the public API. See N11 in
// docs/bug-reports/2026-09-03-full-repo.md.
template <Strategy S>
void NtfsVolume<S>::Impl::AttrRawCallBack(DWORD attType,
                                          const AttrHeaderCommon& ahc,
                                          bool& bDiscard) const
{
  // The caller passes an index below kAttrNums, as described above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  if (attr_raw_call_back_[attType] != nullptr)
  {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    attr_raw_call_back_[attType](ahc, bDiscard);
  }
}

// Clear all Attribute CallBack routines
template <Strategy S>
void NtfsVolume<S>::ClearAttrRawCB() noexcept
{
  for (AttrRawCallback& call_back : impl_->attr_raw_call_back_)
  {
    call_back = nullptr;
  }
}

template <Strategy S>
void NtfsVolume<S>::SetEfsKeyProvider(
    std::shared_ptr<Efs::IEfsKeyProvider> provider) noexcept
{
  impl_->efs_provider_ = std::move(provider);
  impl_->efs_provider_set_ = true;
}

template <Strategy S>
std::shared_ptr<Efs::IEfsKeyProvider> NtfsVolume<S>::GetEfsKeyProvider() const
{
  if (!impl_->efs_provider_set_)
  {
    impl_->efs_provider_set_ = true;
#ifdef _WIN32
    impl_->efs_provider_ = Efs::MakeCertStoreKeyProvider();
#endif
  }
  return impl_->efs_provider_;
}

template <Strategy S>
bool NtfsVolume<S>::SetEfsCipherBackend(Efs::CipherBackend backend) noexcept
{
  if (backend == Efs::CipherBackend::kCryptoPp)
  {
#ifndef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
    return false;
#endif
  }
  else if (backend == Efs::CipherBackend::kBCrypt)
  {
#if !(defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
    return false;
#endif
  }
  impl_->efs_backend_ = backend;
  return true;
}

template <Strategy S>
Efs::CipherBackend NtfsVolume<S>::GetEfsCipherBackend() const noexcept
{
  return impl_->efs_backend_;
}

// Reads $UpCase (MFT record 10). Null when it is missing or unusable.
template <Strategy S>
std::unique_ptr<const UpCaseTable> NtfsVolume<S>::Impl::LoadUpCaseTable() const
{
  const auto upcaseRecord = static_cast<ULONGLONG>(Enum::MftIdx::UPCASE);
  if (!volume_ok_ ||
      !IsMftRangeMapped(upcaseRecord * file_record_size_, file_record_size_))
  {
    return {};
  }

  // Like the volume's other metadata reads, it MUST NOT depend on
  // include_deleted.
  FileRecord<S> record(*self_);
  record.impl_->bypass_deleted_gate_ = true;
  record.SetAttrMask(Mask::DATA);
  if (!record.ParseFileRecord(upcaseRecord) || !record.ParseAttrs())
  {
    return {};
  }

  const AttrBase<S>* data = record.FindStream({});
  if (data == nullptr || data->GetDataSize() < kUpCaseByteCount)
  {
    return {};
  }

  std::vector<BYTE> bytes(kUpCaseByteCount);
  const std::optional<ULONGLONG> len = data->ReadData(0, bytes);
  if (!len || *len != bytes.size())
  {
    return {};
  }

  std::optional<UpCaseTable> table = UpCaseTable::FromBytes(bytes);
  if (!table)
  {
    return {};
  }
  return std::make_unique<const UpCaseTable>(std::move(*table));
}

// Loads $UpCase on first use. A failure is cached: the built-in mapping
// answers every later call.
template <Strategy S>
const UpCaseTable& NtfsVolume<S>::Impl::GetUpCaseTable() const
{
  if (!upcase_loaded_)
  {
    upcase_loaded_ = true;
    upcase_ = LoadUpCaseTable();
    if (!upcase_)
    {
      LogInfo("$UpCase is not usable: names collate by the built-in mapping");
    }
  }
  return upcase_ ? *upcase_ : UpCaseTable::BuiltIn();
}

template class NtfsVolume<Strategy::NO_CACHE>;
template class NtfsVolume<Strategy::FULL_CACHE>;

}  // namespace NtfsBrowser
