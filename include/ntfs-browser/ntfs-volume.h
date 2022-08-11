#pragma once

#include <array>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/file-record.h>

#include <windows.h>
#include <tchar.h>

// OK

namespace NtfsBrowser
{
class AttrBase;

class NtfsVolume
{
 public:
  NtfsVolume(_TCHAR volume);
  NtfsVolume(NtfsVolume&& other) noexcept = delete;
  NtfsVolume(NtfsVolume const& other) = delete;
  NtfsVolume& operator=(NtfsVolume&& other) noexcept = delete;
  NtfsVolume& operator=(NtfsVolume const& other) = delete;
  virtual ~NtfsVolume() = default;

  friend class FileRecord;
  friend class AttrBase;

 private:
  typedef std::unique_ptr<std::remove_pointer<HANDLE>::type,
                          decltype(&::CloseHandle)>
      HandlePtr;
  WORD sector_size_;
  DWORD cluster_size_;
  DWORD file_record_size_;
  DWORD index_block_size_;
  ULONGLONG mft_addr_;
  HandlePtr hvolume_;
  bool volume_ok_;
  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_;
  BYTE version_major_;
  BYTE version_minor_;

  // MFT file records ($MFT file itself) may be fragmented
  // Get $MFT Data attribute to translate FileRecord to correct disk offset
  FileRecord mft_record_;     // $MFT File Record
  const AttrBase* mft_data_;  // $MFT Data Attribute

  bool OpenVolume(_TCHAR volume) noexcept;

 public:
  bool IsVolumeOK() const noexcept;
  std::pair<BYTE, BYTE> GetVersion() const noexcept;
  ULONGLONG GetRecordsCount() const noexcept;

  DWORD GetSectorSize() const noexcept;
  DWORD GetClusterSize() const noexcept;
  DWORD GetFileRecordSize() const noexcept;
  DWORD GetIndexBlockSize() const noexcept;
  ULONGLONG GetMFTAddr() const noexcept;

  bool InstallAttrRawCB(DWORD attrType, AttrRawCallback cb) noexcept;
  void ClearAttrRawCB() noexcept;
};  // NtfsVolume
}  // namespace NtfsBrowser
