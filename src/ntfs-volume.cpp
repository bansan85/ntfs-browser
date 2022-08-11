#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/mask.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr-vol-info.h"
#include "attr-vol-name.h"
#include "data/ntfs-bpb.h"
#include "ntfs-common.h"

// OK

namespace NtfsBrowser
{

///////////////////////////////////////
// NTFS Volume Implementation
///////////////////////////////////////
NtfsVolume::NtfsVolume(_TCHAR volume)
    : sector_size_(0),
      cluster_size_(0),
      file_record_size_(0),
      index_block_size_(0),
      mft_addr_(0),
      hvolume_(HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle)),
      volume_ok_(false),
      version_major_(0),
      version_minor_(0),
      mft_record_(*this),
      mft_data_(nullptr)
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

  std::tie(version_major_, this->version_minor_) =
      reinterpret_cast<const AttrVolInfo*>(vec.front().get())->GetVersion();
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
        reinterpret_cast<const AttrVolName*>(vec2.front().get())->GetName();
    NTFS_TRACE1("NTFS volume name: %ls\n", volname.data());
  }
#endif

  volume_ok_ = true;

  mft_record_.SetAttrMask(Mask::DATA);
  if (mft_record_.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::MFT)))
  {
    if (!mft_record_.ParseAttrs())
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
}

// Open a volume ('a' - 'z', 'A' - 'Z'), get volume handle and BPB
bool NtfsVolume::OpenVolume(_TCHAR volume) noexcept
{
  // Verify parameter
  if (!_istalpha(volume))
  {
    NTFS_TRACE("Volume name error, should be like 'C', 'D'\n");
    return false;
  }

  _TCHAR volumePath[7];
  _sntprintf_s(&volumePath[0], 7, 6, _T("\\\\.\\%c:"), volume);
  volumePath[6] = _T('\0');

  hvolume_ =
      HandlePtr(CreateFileW(&volumePath[0], GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, nullptr),
                &CloseHandle);
  if (hvolume_.get() == INVALID_HANDLE_VALUE)
  {
    NTFS_TRACE1("Cannnot open volume %c\n", (char)volume);
    return false;
  }

  DWORD num = 0;
  Data::NtfsBpb bpb{};

  // Read the first sector (boot sector)
  constexpr DWORD default_sector_size = 512;
  if (ReadFile(hvolume_.get(), &bpb, default_sector_size, &num, nullptr) == 0 ||
      num != default_sector_size)
  {
    NTFS_TRACE("Read boot sector error\n");
    hvolume_ = HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle);
    return false;
  }

  if (strncmp(reinterpret_cast<const char*>(&bpb.Signature[0]), NTFS_SIGNATURE,
              sizeof(bpb.Signature)) != 0)
  {
    NTFS_TRACE("Volume file system is not NTFS\n");
    hvolume_ = HandlePtr(INVALID_HANDLE_VALUE, &CloseHandle);
    return false;
  }

  // Log important volume parameters

  sector_size_ = bpb.BytesPerSector;
  NTFS_TRACE1("Sector Size = %u bytes\n", sector_size_);

  cluster_size_ = sector_size_ * bpb.SectorsPerCluster;
  NTFS_TRACE1("Cluster Size = %u bytes\n", cluster_size_);

  char sz = static_cast<char>(bpb.ClustersPerFileRecord);
  if (sz > 0)
  {
    file_record_size_ = cluster_size_ * sz;
  }
  else
  {
    file_record_size_ = 1U << static_cast<unsigned char>(-sz);
  }
  NTFS_TRACE1("FileRecord Size = %u bytes\n", file_record_size_);

  sz = static_cast<char>(bpb.ClustersPerIndexBlock);
  if (sz > 0)
  {
    index_block_size_ = cluster_size_ * sz;
  }
  else
  {
    index_block_size_ = 1U << static_cast<unsigned char>(-sz);
  }
  NTFS_TRACE1("IndexBlock Size = %u bytes\n", index_block_size_);

  mft_addr_ = bpb.LCN_MFT * cluster_size_;
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

DWORD NtfsVolume::GetSectorSize() const noexcept { return sector_size_; }

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

// Install Attribute CallBack routines for the whole Volume
bool NtfsVolume::InstallAttrRawCB(DWORD attrType, AttrRawCallback cb) noexcept
{
  const DWORD atIdx = ATTR_INDEX(attrType);
  if (atIdx < kAttrNums)
  {
    attr_raw_call_back_[atIdx] = cb;
    return true;
  }
  return false;
}

// Clear all Attribute CallBack routines
void NtfsVolume::ClearAttrRawCB() noexcept
{
  for (size_t i = 0; i < kAttrNums; i++)
  {
    attr_raw_call_back_[i] = nullptr;
  }
}

}  // namespace NtfsBrowser