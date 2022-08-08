#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/mask.h>
#include "ntfs-common.h"
#include "attr-vol-info.h"
#include "attr-vol-name.h"
#include <ntfs-browser/mft-idx.h>
#include "data/ntfs-bpb.h"

namespace NtfsBrowser
{

///////////////////////////////////////
// NTFS Volume Implementation
///////////////////////////////////////
NtfsVolume::NtfsVolume(_TCHAR volume)
{
  hvolume_ = INVALID_HANDLE_VALUE;
  volume_ok_ = FALSE;
  mft_record_ = nullptr;
  mft_data_ = nullptr;
  ClearAttrRawCB();

  if (!OpenVolume(volume)) return;

  // Verify NTFS volume version (must >= 3.0)

  FileRecord vol(*this);
  vol.SetAttrMask(Mask::VOLUME_NAME | Mask::VOLUME_INFORMATION);
  if (!vol.ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::VOLUME))) return;

  if (!vol.ParseAttrs()) return;
  const auto& vec =
      vol.getAttr(static_cast<DWORD>(AttrType::VOLUME_INFORMATION));
  if (vec.empty()) return;

  std::tie(version_major_, this->version_minor_) =
      ((AttrVolInfo*)vec.front())->GetVersion();
  NTFS_TRACE2("NTFS volume version: %u.%u\n", version_major_, version_minor_);
  if (version_major_ < 3)  // NT4 ?
    return;

#ifdef _DEBUG
  const auto& vec2 = vol.getAttr(static_cast<DWORD>(AttrType::VOLUME_NAME));
  if (!vec2.empty())
  {
    std::wstring_view volname{((AttrVolName*)vec2.front())->GetName()};
    NTFS_TRACE1("NTFS volume name: %ls\n", volname.data());
  }
#endif

  volume_ok_ = TRUE;

  mft_record_ = new FileRecord(*this);
  mft_record_->SetAttrMask(Mask::DATA);
  if (mft_record_->ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::MFT)))
  {
    if (!mft_record_->ParseAttrs())
    {
      delete mft_record_;
      mft_record_ = nullptr;
      return;
    }
    const std::vector<AttrBase*>& vec3 =
        mft_record_->getAttr(static_cast<DWORD>(AttrType::DATA));
    if (vec3.empty())
    {
      delete mft_record_;
      mft_record_ = nullptr;
    }
    else
    {
      mft_data_ = vec3.front();
    }
  }
}

NtfsVolume::~NtfsVolume()
{
  if (hvolume_ != INVALID_HANDLE_VALUE) CloseHandle(hvolume_);

  if (mft_record_) delete mft_record_;
}

// Open a volume ('a' - 'z', 'A' - 'Z'), get volume handle and BPB
BOOL NtfsVolume::OpenVolume(_TCHAR volume)
{
  // Verify parameter
  if (!_istalpha(volume))
  {
    NTFS_TRACE("Volume name error, should be like 'C', 'D'\n");
    return FALSE;
  }

  _TCHAR volumePath[7];
  _sntprintf(volumePath, 6, _T("\\\\.\\%c:"), volume);
  volumePath[6] = _T('\0');

  hvolume_ =
      CreateFileW(volumePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, nullptr);
  if (hvolume_ != INVALID_HANDLE_VALUE)
  {
    DWORD num;
    Data::NtfsBpb bpb;

    // Read the first sector (boot sector)
    if (ReadFile(hvolume_, &bpb, 512, &num, nullptr) && num == 512)
    {
      if (strncmp((const char*)bpb.Signature, NTFS_SIGNATURE, 8) == 0)
      {
        // Log important volume parameters

        sector_size_ = bpb.BytesPerSector;
        NTFS_TRACE1("Sector Size = %u bytes\n", sector_size_);

        cluster_size_ = sector_size_ * bpb.SectorsPerCluster;
        NTFS_TRACE1("Cluster Size = %u bytes\n", cluster_size_);

        int sz = (char)bpb.ClustersPerFileRecord;
        if (sz > 0)
          file_record_size_ = cluster_size_ * sz;
        else
          file_record_size_ = 1 << (-sz);
        NTFS_TRACE1("FileRecord Size = %u bytes\n", file_record_size_);

        sz = (char)bpb.ClustersPerIndexBlock;
        if (sz > 0)
          index_block_size_ = cluster_size_ * sz;
        else
          index_block_size_ = 1 << (-sz);
        NTFS_TRACE1("IndexBlock Size = %u bytes\n", index_block_size_);

        mft_addr_ = bpb.LCN_MFT * cluster_size_;
        NTFS_TRACE1("MFT address = 0x%016I64X\n", mft_addr_);
      }
      else
      {
        NTFS_TRACE("Volume file system is not NTFS\n");
        goto IOError;
      }
    }
    else
    {
      NTFS_TRACE("Read boot sector error\n");
      goto IOError;
    }
  }
  else
  {
    NTFS_TRACE1("Cannnot open volume %c\n", (char)volume);
  IOError:
    if (hvolume_ != INVALID_HANDLE_VALUE)
    {
      CloseHandle(hvolume_);
      hvolume_ = INVALID_HANDLE_VALUE;
    }
    return FALSE;
  }

  return TRUE;
}

// Check if Volume is successfully opened
BOOL NtfsVolume::IsVolumeOK() const { return volume_ok_; }

// Get NTFS volume version
std::pair<BYTE, BYTE> NtfsVolume::GetVersion() const
{
  return {version_major_, version_minor_};
}

// Get File Record count
ULONGLONG NtfsVolume::GetRecordsCount() const
{
  return (mft_data_->GetDataSize() / file_record_size_);
}

// Get BPB information

DWORD NtfsVolume::GetSectorSize() const { return sector_size_; }

DWORD NtfsVolume::GetClusterSize() const { return cluster_size_; }

DWORD NtfsVolume::GetFileRecordSize() const { return file_record_size_; }

DWORD NtfsVolume::GetIndexBlockSize() const { return index_block_size_; }

// Get MFT starting address
ULONGLONG NtfsVolume::GetMFTAddr() const { return mft_addr_; }

// Install Attribute CallBack routines for the whole Volume
BOOL NtfsVolume::InstallAttrRawCB(DWORD attrType, AttrRawCallback cb)
{
  DWORD atIdx = ATTR_INDEX(attrType);
  if (atIdx < kAttrNums)
  {
    attr_raw_call_back_[atIdx] = cb;
    return TRUE;
  }
  else
    return FALSE;
}

// Clear all Attribute CallBack routines
void NtfsVolume::ClearAttrRawCB()
{
  for (int i = 0; i < kAttrNums; i++) attr_raw_call_back_[i] = nullptr;
}

}  // namespace NtfsBrowser