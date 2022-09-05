#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr-vol-info.h"
#include "attr-vol-name.h"
#include "data/ntfs-bpb.h"
#include "ntfs-common.h"

namespace NtfsBrowser
{

NtfsVolume::NtfsVolume(_TCHAR volume, FileReader::Strategy strategy)
    : mft_record_(*this), volume_{strategy}
{
  ClearAttrRawCB();

  if (!OpenVolume(volume))
  {
    return;
  }

  // Verify NTFS volume version (must >= 3.0)

  FileRecord vol(*this);
  vol.SetAttrMask(Mask::VOLUME_NAME | Mask::VOLUME_INFORMATION);
  if (!vol.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::VOLUME)))
  {
    return;
  }

  if (!vol.ParseAttrs())
  {
    return;
  }
  const auto& vec =
      vol.getAttr(static_cast<DWORD>(AttrType::VOLUME_INFORMATION));
  if (vec.empty())
  {
    return;
  }

  if (strategy == FileReader::Strategy::NO_CACHE)
  {
    std::tie(version_major_, version_minor_) =
        reinterpret_cast<const AttrVolInfo<AttrResidentHeavy>*>(
            vec.front().get())
            ->GetVersion();
  }
  else
  {
    std::tie(version_major_, version_minor_) =
        reinterpret_cast<const AttrVolInfo<AttrResidentLight>*>(
            vec.front().get())
            ->GetVersion();
  }
  NTFS_TRACE2("NTFS volume version: %u.%u\n", version_major_, version_minor_);
  if (version_major_ < 3)  // NT4 ?
  {
    return;
  }

#ifdef _DEBUG
  const auto& vec2 = vol.getAttr(static_cast<DWORD>(AttrType::VOLUME_NAME));
  if (!vec2.empty())
  {
    const std::wstring_view volname =
        reinterpret_cast<const AttrVolName<AttrResidentLight>*>(
            vec2.front().get())
            ->GetName();
    NTFS_TRACE1("NTFS volume name: %ls\n", volname.data());
  }
#endif

  volume_ok_ = true;

  mft_record_.SetAttrMask(Mask::DATA);
  if (!mft_record_.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::MFT)) ||
      !mft_record_.ParseAttrs())
  {
    return;
  }

  const std::vector<std::unique_ptr<AttrBase>>& vec3 =
      mft_record_.getAttr(static_cast<DWORD>(AttrType::DATA));
  if (!vec3.empty())
  {
    mft_data_ = vec3.front().get();
  }
}

// Open a volume ('a' - 'z', 'A' - 'Z'), get volume handle and BPB
bool NtfsVolume::OpenVolume(_TCHAR volume)
{
  // Verify parameter
  if (!_istalpha(volume))
  {
    NTFS_TRACE("Volume name error, should be like 'C', 'D'\n");
    return false;
  }

  std::array<_TCHAR, 7> volumePath;
  _sntprintf_s(volumePath.data(), 7, 6, _T("\\\\.\\%c:"), volume);
  volumePath[6] = _T('\0');

  if (!volume_.Open(volumePath.data()))
  {
    NTFS_TRACE1("Cannnot open volume %c\n", (char)volume);
    return false;
  }

  DWORD num = 0;

  // Read the first sector (boot sector)
  constexpr DWORD default_sector_size = 512;
  LARGE_INTEGER frAddr{.QuadPart = 0};
  std::optional<std::span<const BYTE>> bpb_buffer =
      volume_.Read(frAddr, default_sector_size);
  if (!bpb_buffer)
  {
    NTFS_TRACE("Read boot sector error\n");
    return false;
  }
  auto bpb = reinterpret_cast<const Data::NtfsBpb*>(bpb_buffer->data());

  if (strncmp(reinterpret_cast<const char*>(&bpb->signature[0]), NTFS_SIGNATURE,
              sizeof(bpb->signature)) != 0)
  {
    NTFS_TRACE("Volume file system is not NTFS\n");
    return false;
  }

  // Log important volume parameters

  sector_size_ = bpb->bytes_per_sector;
  NTFS_TRACE1("Sector Size = %u bytes\n", sector_size_);

  cluster_size_ = sector_size_ * bpb->sectors_per_cluster;
  NTFS_TRACE1("Cluster Size = %u bytes\n", cluster_size_);
  cluster_buffer_.resize(cluster_size_);

  char sz = static_cast<char>(bpb->clusters_per_file_record);
  if (sz > 0)
  {
    file_record_size_ = cluster_size_ * sz;
  }
  else
  {
    file_record_size_ = 1U << static_cast<unsigned char>(-sz);
  }
  NTFS_TRACE1("FileRecord Size = %u bytes\n", file_record_size_);
  file_record_buffer_.resize(file_record_size_);

  sz = static_cast<char>(bpb->clusters_per_index_block);
  if (sz > 0)
  {
    index_block_size_ = cluster_size_ * sz;
  }
  else
  {
    index_block_size_ = 1U << static_cast<unsigned char>(-sz);
  }
  NTFS_TRACE1("IndexBlock Size = %u bytes\n", index_block_size_);

  mft_addr_ = bpb->lcn_mft * cluster_size_;
  NTFS_TRACE1("MFT address = 0x%016I64X\n", mft_addr_);

  return true;
}

// Check if Volume is successfully opened
bool NtfsVolume::IsVolumeOK() const noexcept { return volume_ok_; }

// Get NTFS volume version
std::pair<BYTE, BYTE> NtfsVolume::GetVersion() const noexcept
{
  return {version_major_, version_minor_};
}

// Get File Record count
ULONGLONG NtfsVolume::GetRecordsCount() const noexcept
{
  return (mft_data_->GetDataSize() / file_record_size_);
}

// Get BPB information

WORD NtfsVolume::GetSectorSize() const noexcept { return sector_size_; }

DWORD NtfsVolume::GetClusterSize() const noexcept { return cluster_size_; }

DWORD NtfsVolume::GetFileRecordSize() const noexcept
{
  return file_record_size_;
}

DWORD NtfsVolume::GetIndexBlockSize() const noexcept
{
  return index_block_size_;
}

// Get MFT starting address
ULONGLONG NtfsVolume::GetMFTAddr() const noexcept { return mft_addr_; }

std::span<BYTE> NtfsVolume::GetClusterBuffer() const noexcept
{
  return {cluster_buffer_.data(), cluster_buffer_.size()};
}

std::span<BYTE> NtfsVolume::GetFileRecordBuffer() const noexcept
{
  return {file_record_buffer_.data(), file_record_buffer_.size()};
}

std::optional<std::span<const BYTE>> NtfsVolume::Read(LARGE_INTEGER& addr,
                                                      DWORD length) const
{
  return volume_.Read(addr, length);
}

// Install Attribute CallBack routines for the whole Volume
bool NtfsVolume::InstallAttrRawCB(DWORD attrType, AttrRawCallback cb) noexcept
{
  const DWORD atIdx = ATTR_INDEX(attrType);
  if (atIdx >= kAttrNums)
  {
    return false;
  }

  attr_raw_call_back_[atIdx] = cb;
  return true;
}

void NtfsVolume::AttrRawCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                                 bool& bDiscard) const
{
  if (attr_raw_call_back_[attType] != nullptr)
  {
    attr_raw_call_back_[attType](ahc, bDiscard);
  }
}

// Clear all Attribute CallBack routines
void NtfsVolume::ClearAttrRawCB() noexcept
{
  for (AttrRawCallback& call_back : attr_raw_call_back_)
  {
    call_back = nullptr;
  }
}

}  // namespace NtfsBrowser