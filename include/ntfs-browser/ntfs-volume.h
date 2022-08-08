#pragma once

#include <array>

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
  WORD sector_size_;
  DWORD cluster_size_;
  DWORD file_record_size_;
  DWORD index_block_size_;
  ULONGLONG mft_addr_;
  HANDLE hvolume_;
  BOOL volume_ok_;
  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_;
  BYTE version_major_;
  BYTE version_minor_;

  // MFT file records ($MFT file itself) may be fragmented
  // Get $MFT Data attribute to translate FileRecord to correct disk offset
  FileRecord* mft_record_;    // $MFT File Record
  const AttrBase* mft_data_;  // $MFT Data Attribute

  BOOL OpenVolume(_TCHAR volume);

 public:
  BOOL IsVolumeOK() const;
  std::pair<BYTE, BYTE> GetVersion() const;
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
