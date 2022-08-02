#pragma once

#include <ntfs-browser/data/attr-defines.h>

#include <windows.h>
#include <tchar.h>

namespace NtfsBrowser
{
class FileRecord;
class AttrBase;

class NtfsVolume
{
 public:
  NtfsVolume(_TCHAR volume);
  virtual ~NtfsVolume();

  friend class FileRecord;
  friend class AttrBase;

 private:
  WORD SectorSize;
  DWORD ClusterSize;
  DWORD FileRecordSize;
  DWORD IndexBlockSize;
  ULONGLONG MFTAddr;
  HANDLE hVolume;
  BOOL VolumeOK;
  AttrRawCallback AttrRawCallBack[kAttrNums];
  WORD Version;

  // MFT file records ($MFT file itself) may be fragmented
  // Get $MFT Data attribute to translate FileRecord to correct disk offset
  FileRecord* MFTRecord;    // $MFT File Record
  const AttrBase* MFTData;  // $MFT Data Attribute

  BOOL OpenVolume(_TCHAR volume);

 public:
  BOOL IsVolumeOK() const;
  WORD GetVersion() const;
  ULONGLONG GetRecordsCount() const;

  DWORD GetSectorSize() const;
  DWORD GetClusterSize() const;
  DWORD GetFileRecordSize() const;
  DWORD GetIndexBlockSize() const;
  ULONGLONG GetMFTAddr() const;

  BOOL InstallAttrRawCB(DWORD attrType, AttrRawCallback cb);
  void ClearAttrRawCB();
};  // NtfsVolume
}  // namespace NtfsBrowser
