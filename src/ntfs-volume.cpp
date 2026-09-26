#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr-non-resident.h"
#include "attr-vol-info.h"
#include "attr-vol-name.h"
#include "attr/attribute-list.h"
#include "data/file-record-header.h"
#include "data/index-block.h"
#include "data/ntfs-bpb.h"
#include "file-reader.h"
#include "ntfs-common.h"
#include "utf.h"

namespace NtfsBrowser
{
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

// Caps tracked $MFT DATA-continuation refs against a forged $ATTRIBUTE_LIST.
constexpr size_t kMaxMftAttrListEntries = 65536;

}  // namespace

#ifdef _WIN32
template <Strategy S>
NtfsVolume<S>::NtfsVolume(_TCHAR volume, const VolumeOptions& options)
    : volume_(std::make_unique<FileReader<S>>()),
      options_(options),
      mft_record_(*this)
{
  ClearAttrRawCB();

  if (OpenVolume(volume))
  {
    Init();
  }
}

template <Strategy S>
NtfsVolume<S>::NtfsVolume(std::wstring_view path, const VolumeOptions& options)
    : volume_(std::make_unique<FileReader<S>>()),
      options_(options),
      mft_record_(*this)
{
  ClearAttrRawCB();

  if (OpenVolume(path))
  {
    Init();
  }
}
#endif

template <Strategy S>
NtfsVolume<S>::NtfsVolume(std::unique_ptr<IDiskReader> reader,
                          const VolumeOptions& options)
    : volume_(std::make_unique<FileReader<S>>()),
      options_(options),
      mft_record_(*this)
{
  ClearAttrRawCB();

  if (OpenVolume(std::move(reader)))
  {
    Init();
  }
}

template <Strategy S>
NtfsVolume<S>::~NtfsVolume() = default;

// Verify NTFS volume version (must >= 3.0) and locate $MFT's Data attribute
template <Strategy S>
void NtfsVolume<S>::Init()
{
  // The volume's own metadata reads always see their own content, whatever
  // include_deleted says: they are not the caller's traversal of the
  // filesystem, and a freed $Volume/$MFT would otherwise make the whole
  // volume unreadable.
  mft_record_.bypass_deleted_gate_ = true;

  FileRecord vol(*this);
  vol.bypass_deleted_gate_ = true;
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
    if (S == Strategy::NO_CACHE)
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
  mft_record_.attr_mask_ = Mask::DATA;
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
  TryAddMftExtent(*baseExtent, (std::numeric_limits<ULONGLONG>::max)());

  // Must run after mft_data_/mft_extents_ are set, so it can use them.
  ResolveMftDataExtents();

  // Reported OK only once mft_data_ is actually assigned.
  volume_ok_ = true;
}

// Resolves $MFT's own DATA continuations named by its $ATTRIBUTE_LIST, as a
// fixed point since one entry can depend on an extent only another reveals.
template <Strategy S>
void NtfsVolume<S>::ResolveMftDataExtents()
{
  // Isolated from mft_record_; resolve_attr_list_ = false skips AttrList.
  FileRecord<S> listRecord(*this);
  listRecord.attr_mask_ = Mask::ATTRIBUTE_LIST;
  listRecord.resolve_attr_list_ = false;
  listRecord.bypass_deleted_gate_ = true;

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
  const ULONGLONG selfRef = *listRecord.GetFileReference();

  // Collects (record_ref, start_vcn) for each DATA entry, deduped and capped.
  std::vector<std::pair<ULONGLONG, ULONGLONG>> pending;
  {
    std::unordered_set<ULONGLONG> seen;
    ULONGLONG offset = 0;
    Attr::AttributeList entry{};
    std::optional<ULONGLONG> len;
    while (
        pending.size() < kMaxMftAttrListEntries &&
        (len = rawList.ReadData(offset, {reinterpret_cast<BYTE*>(&entry),
                                         Attr::kAttributeListEntryHeaderSize})))
    {
      if (*len != Attr::kAttributeListEntryHeaderSize ||
          !IsValidAttrType(entry.attr_type))
      {
        break;
      }

      const ULONGLONG recordRef = entry.base_ref.segment_number;
      if (entry.attr_type == AttrType::DATA && entry.name_length == 0 &&
          recordRef != selfRef && seen.insert(recordRef).second)
      {
        pending.emplace_back(recordRef, entry.start_vcn);
      }

      if (entry.record_size == 0)
      {
        break;
      }
      offset += entry.record_size;
    }
  }

  // Attempts each ref once reachable, dropping it either way to bound reads.
  while (!pending.empty())
  {
    std::vector<std::pair<ULONGLONG, ULONGLONG>> stillPending;
    bool attemptedAny = false;

    for (const auto& [recordRef, startVcn] : pending)
    {
      const bool reachable =
          recordRef < static_cast<ULONGLONG>(Enum::MftIdx::USER) ||
          IsMftRangeMapped(recordRef * file_record_size_, file_record_size_);
      if (!reachable)
      {
        stillPending.emplace_back(recordRef, startVcn);
        continue;
      }
      attemptedAny = true;

      mft_extension_records_.emplace_back(*this);
      FileRecord<S>& ext = mft_extension_records_.back();
      ext.attr_mask_ = Mask::DATA;
      ext.bypass_deleted_gate_ = true;

      if (ext.ParseFileRecord(recordRef) && ext.ParseAttrs())
      {
        for (const std::unique_ptr<AttrBase<S>>& attr :
             ext.getAttr(AttrType::DATA))
        {
          if (attr->IsNonResident())
          {
            TryAddMftExtent(*attr, startVcn);
          }
        }
      }
      else
      {
        mft_extension_records_.pop_back();
        LogWarn("$MFT DATA continuation in record {} could not be resolved",
                recordRef);
      }
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

// Rejects attr if named, VCN-inverted, mismatched with expectedStartVcn, or
// overlapping; otherwise inserts it into mft_extents_ in sorted order.
template <Strategy S>
void NtfsVolume<S>::TryAddMftExtent(const AttrBase<S>& attr,
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

  if (expectedStartVcn != (std::numeric_limits<ULONGLONG>::max)() &&
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
bool NtfsVolume<S>::IsMftRangeMapped(ULONGLONG byteOffset,
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
const typename NtfsVolume<S>::MftExtent*
    NtfsVolume<S>::FindMftExtent(ULONGLONG vcn) const noexcept
{
  const auto it = std::upper_bound(mft_extents_.begin(), mft_extents_.end(),
                                   vcn, [](ULONGLONG v, const MftExtent& extent)
                                   { return v < extent.start_vcn; });
  if (it == mft_extents_.begin())
  {
    return nullptr;
  }
  const MftExtent& candidate = *std::prev(it);
  return (candidate.last_vcn >= vcn) ? &candidate : nullptr;
}

// Reads $MFT's DATA attribute at a byte offset, following extent
// boundaries transparently when it is split across extension records.
template <Strategy S>
std::optional<ULONGLONG>
    NtfsVolume<S>::ReadMftData(ULONGLONG offset, std::span<BYTE> buffer) const
{
  ULONGLONG totalRead = 0;
  BYTE* buf = buffer.data();
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

    const std::optional<ULONGLONG> len = nonResident->ReadExtentData(
        currentOffset - extentStartByte, {buf, static_cast<size_t>(toRead)});
    if (!len || *len != toRead)
    {
      return {};
    }

    buf += toRead;
    currentOffset += toRead;
    remaining -= toRead;
    totalRead += toRead;
  }

  return totalRead;
}

#ifdef _WIN32
// Open a volume ('a' - 'z', 'A' - 'Z'), get volume handle and BPB
template <Strategy S>
bool NtfsVolume<S>::OpenVolume(_TCHAR volume)
{
  // Verify parameter
  if (!_istalpha(volume))
  {
    LogError("Volume name error, should be like 'C', 'D'");
    return false;
  }

  std::array<_TCHAR, 7> volumePath;
  _sntprintf_s(volumePath.data(), 7, 6, _T("\\\\.\\%c:"), volume);
  volumePath[6] = _T('\0');

  return OpenVolume(std::wstring_view(volumePath.data()));
}

// Open an arbitrary device/image path, get volume handle and BPB
template <Strategy S>
bool NtfsVolume<S>::OpenVolume(std::wstring_view path)
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
bool NtfsVolume<S>::OpenVolume(std::unique_ptr<IDiskReader> reader)
{
  volume_ = std::make_unique<FileReader<S>>(std::move(reader));

  return ParseBootSector();
}

// Read the first sector (boot sector) and derive volume geometry from it
template <Strategy S>
bool NtfsVolume<S>::ParseBootSector()
{
  constexpr DWORD default_sector_size = 512;
  LARGE_INTEGER frAddr{.QuadPart = 0};
  std::optional<std::span<const BYTE>> bpb_buffer =
      volume_->Read(frAddr, default_sector_size);
  if (!bpb_buffer)
  {
    LogError("Read boot sector error");
    return false;
  }
  auto bpb = reinterpret_cast<const Data::NtfsBpb*>(bpb_buffer->data());

  if (strncmp(reinterpret_cast<const char*>(&bpb->signature[0]), NTFS_SIGNATURE,
              sizeof(bpb->signature)) != 0)
  {
    LogWarn("Volume file system is not NTFS");
    return false;
  }

  // Log important volume parameters

  sector_size_ = bpb->bytes_per_sector;
  LogInfo("Sector Size = {} bytes", sector_size_);

  // Sector size must be >= 2 to prevent integer underflow in fixup-patch pointer arithmetic.
  if (sector_size_ < sizeof(WORD))
  {
    LogError("Sector Size must be at least 2 bytes");
    return false;
  }

  cluster_size_ = sector_size_ * bpb->sectors_per_cluster;
  LogInfo("Cluster Size = {} bytes", cluster_size_);

  if (cluster_size_ == 0)
  {
    LogError("Cluster Size can't be null");
    return false;
  }
  cluster_buffer_.resize(cluster_size_);

  char sz = static_cast<char>(bpb->clusters_per_file_record);

  // Rejects an sz magnitude that would shift 1U by 32 or more (undefined
  // behaviour), or yield a file_record_size_ no real volume could have.
  if (sz < -12 || sz > 8)
  {
    LogError("clusters_per_file_record magnitude out of range");
    return false;
  }

  if (sz > 0)
  {
    file_record_size_ = cluster_size_ * sz;
  }
  else
  {
    file_record_size_ = 1U << static_cast<unsigned char>(-sz);
  }
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

  sz = static_cast<char>(bpb->clusters_per_index_block);

  // Rejects an sz magnitude that would shift 1U by 32 or more (undefined
  // behaviour), or yield an index_block_size_ no real volume could have.
  if (sz < -12 || sz > 8)
  {
    LogError("clusters_per_index_block magnitude out of range");
    return false;
  }

  if (sz > 0)
  {
    index_block_size_ = cluster_size_ * sz;
  }
  else
  {
    index_block_size_ = 1U << static_cast<unsigned char>(-sz);
  }
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
      bpb->lcn_mft > (std::numeric_limits<ULONGLONG>::max)() / cluster_size_;
  mft_addr_ = mft_addr_overflows ? (std::numeric_limits<ULONGLONG>::max)()
                                 : bpb->lcn_mft * cluster_size_;
  LogInfo("MFT address = 0x{:016X}", mft_addr_);

  // Leaves headroom for the per-record byte offset added to mft_addr_
  // later, before it is narrowed to a LONGLONG.
  constexpr ULONGLONG kMaxPlausibleMftAddr =
      (std::numeric_limits<LONGLONG>::max)() / 2;

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
  return volume_ok_;
}

template <Strategy S>
const VolumeOptions& NtfsVolume<S>::GetOptions() const noexcept
{
  return options_;
}

// Get NTFS volume version
template <Strategy S>
std::pair<BYTE, BYTE> NtfsVolume<S>::GetVersion() const noexcept
{
  return {version_major_, version_minor_};
}

// Get File Record count
template <Strategy S>
ULONGLONG NtfsVolume<S>::GetRecordsCount() const noexcept
{
  // noexcept: must not crash if called before/without checking IsVolumeOK().
  if (mft_data_ == nullptr)
  {
    return 0;
  }

  return (mft_data_->GetDataSize() / file_record_size_);
}

// Get BPB information
template <Strategy S>
WORD NtfsVolume<S>::GetSectorSize() const noexcept
{
  return sector_size_;
}

template <Strategy S>
DWORD NtfsVolume<S>::GetClusterSize() const noexcept
{
  return cluster_size_;
}

template <Strategy S>
DWORD NtfsVolume<S>::GetFileRecordSize() const noexcept
{
  return file_record_size_;
}

template <Strategy S>
DWORD NtfsVolume<S>::GetIndexBlockSize() const noexcept
{
  return index_block_size_;
}

// Get MFT starting address
template <Strategy S>
ULONGLONG NtfsVolume<S>::GetMFTAddr() const noexcept
{
  return mft_addr_;
}

template <Strategy S>
std::span<BYTE> NtfsVolume<S>::GetClusterBuffer() const noexcept
{
  return {cluster_buffer_.data(), cluster_buffer_.size()};
}

template <Strategy S>
std::optional<std::span<const BYTE>> NtfsVolume<S>::Read(LARGE_INTEGER& addr,
                                                         DWORD length) const
{
  return volume_->Read(addr, length);
}

template <Strategy S>
bool NtfsVolume<S>::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const
{
  return volume_->ReadInto(addr, dest);
}

// Install Attribute CallBack routines for the whole Volume
template <Strategy S>
bool NtfsVolume<S>::InstallAttrRawCB(AttrType attrType,
                                     AttrRawCallback cb) noexcept
{
  const DWORD atIdx = ATTR_INDEX(attrType);
  if (atIdx >= kAttrNums)
  {
    return false;
  }

  attr_raw_call_back_[atIdx] = cb;
  return true;
}

template <Strategy S>
void NtfsVolume<S>::AttrRawCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                                    bool& bDiscard) const
{
  if (attr_raw_call_back_[attType] != nullptr)
  {
    attr_raw_call_back_[attType](ahc, bDiscard);
  }
}

// Clear all Attribute CallBack routines
template <Strategy S>
void NtfsVolume<S>::ClearAttrRawCB() noexcept
{
  for (AttrRawCallback& call_back : attr_raw_call_back_)
  {
    call_back = nullptr;
  }
}

template <Strategy S>
void NtfsVolume<S>::SetEfsKeyProvider(
    std::shared_ptr<Efs::IEfsKeyProvider> provider) noexcept
{
  efs_provider_ = std::move(provider);
  efs_provider_set_ = true;
}

template <Strategy S>
std::shared_ptr<Efs::IEfsKeyProvider> NtfsVolume<S>::GetEfsKeyProvider() const
{
  if (!efs_provider_set_)
  {
    efs_provider_set_ = true;
#ifdef _WIN32
    efs_provider_ = Efs::MakeCertStoreKeyProvider();
#endif
  }
  return efs_provider_;
}

template class NtfsVolume<Strategy::NO_CACHE>;
template class NtfsVolume<Strategy::FULL_CACHE>;

}  // namespace NtfsBrowser
