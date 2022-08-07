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
  hVolume = INVALID_HANDLE_VALUE;
  VolumeOK = FALSE;
  MFTRecord = nullptr;
  MFTData = nullptr;
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

  std::tie(VersionMajor, this->VersionMinor) =
      ((AttrVolInfo*)vec.front())->GetVersion();
  NTFS_TRACE2("NTFS volume version: %u.%u\n", VersionMajor, VersionMinor);
  if (VersionMajor < 3)  // NT4 ?
    return;

#ifdef _DEBUG
  const auto& vec2 = vol.getAttr(static_cast<DWORD>(AttrType::VOLUME_NAME));
  if (!vec2.empty())
  {
    std::wstring_view volname{((AttrVolName*)vec2.front())->GetName()};
    NTFS_TRACE1("NTFS volume name: %ls\n", volname.data());
  }
#endif

  VolumeOK = TRUE;

  MFTRecord = new FileRecord(*this);
  MFTRecord->SetAttrMask(Mask::DATA);
  if (MFTRecord->ParseFileRecord(static_cast<DWORD>(Enum::MftIdx::MFT)))
  {
    if (!MFTRecord->ParseAttrs())
    {
      delete MFTRecord;
      MFTRecord = nullptr;
      return;
    }
    const std::vector<AttrBase*>& vec3 =
        MFTRecord->getAttr(static_cast<DWORD>(AttrType::DATA));
    if (vec3.empty())
    {
      delete MFTRecord;
      MFTRecord = nullptr;
    }
    else
    {
      MFTData = vec3.front();
    }
  }
}

NtfsVolume::~NtfsVolume()
{
  if (hVolume != INVALID_HANDLE_VALUE) CloseHandle(hVolume);

  if (MFTRecord) delete MFTRecord;
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

  hVolume =
      CreateFile(volumePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, nullptr);
  if (hVolume != INVALID_HANDLE_VALUE)
  {
    DWORD num;
    Data::NtfsBpb bpb;

    // Read the first sector (boot sector)
    if (ReadFile(hVolume, &bpb, 512, &num, nullptr) && num == 512)
    {
      if (strncmp((const char*)bpb.Signature, NTFS_SIGNATURE, 8) == 0)
      {
        // Log important volume parameters

        SectorSize = bpb.BytesPerSector;
        NTFS_TRACE1("Sector Size = %u bytes\n", SectorSize);

        ClusterSize = SectorSize * bpb.SectorsPerCluster;
        NTFS_TRACE1("Cluster Size = %u bytes\n", ClusterSize);

        int sz = (char)bpb.ClustersPerFileRecord;
        if (sz > 0)
          FileRecordSize = ClusterSize * sz;
        else
          FileRecordSize = 1 << (-sz);
        NTFS_TRACE1("FileRecord Size = %u bytes\n", FileRecordSize);

        sz = (char)bpb.ClustersPerIndexBlock;
        if (sz > 0)
          IndexBlockSize = ClusterSize * sz;
        else
          IndexBlockSize = 1 << (-sz);
        NTFS_TRACE1("IndexBlock Size = %u bytes\n", IndexBlockSize);

        MFTAddr = bpb.LCN_MFT * ClusterSize;
        NTFS_TRACE1("MFT address = 0x%016I64X\n", MFTAddr);
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
    if (hVolume != INVALID_HANDLE_VALUE)
    {
      CloseHandle(hVolume);
      hVolume = INVALID_HANDLE_VALUE;
    }
    return FALSE;
  }

  return TRUE;
}

// Check if Volume is successfully opened
BOOL NtfsVolume::IsVolumeOK() const { return VolumeOK; }

// Get NTFS volume version
std::pair<BYTE, BYTE> NtfsVolume::GetVersion() const
{
  return {VersionMajor, VersionMinor};
}

// Get File Record count
ULONGLONG NtfsVolume::GetRecordsCount() const
{
  return (MFTData->GetDataSize() / FileRecordSize);
}

// Get BPB information

DWORD NtfsVolume::GetSectorSize() const { return SectorSize; }

DWORD NtfsVolume::GetClusterSize() const { return ClusterSize; }

DWORD NtfsVolume::GetFileRecordSize() const { return FileRecordSize; }

DWORD NtfsVolume::GetIndexBlockSize() const { return IndexBlockSize; }

// Get MFT starting address
ULONGLONG NtfsVolume::GetMFTAddr() const { return MFTAddr; }

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