#pragma once

#include <array>

#include <tchar.h>
#include <windows.h>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/file-record.h>

namespace NtfsBrowser
{
class AttrBase;

class NtfsVolume
{
 public:
  explicit NtfsVolume(_TCHAR volume);
  NtfsVolume(NtfsVolume&& other) noexcept = delete;
  NtfsVolume(NtfsVolume const& other) = delete;
  NtfsVolume& operator=(NtfsVolume&& other) noexcept = delete;
  NtfsVolume& operator=(NtfsVolume const& other) = delete;
  virtual ~NtfsVolume() = default;

  friend class FileRecord;
  friend class AttrBase;

 private:
  using HandlePtr = std::unique_ptr<std::remove_pointer<HANDLE>::type,
                                    decltype(&::CloseHandle)>;
  WORD sector_size_{0};
  DWORD cluster_size_{0};
  DWORD file_record_size_{0};
  DWORD index_block_size_{0};
  ULONGLONG mft_addr_{0};
  HandlePtr hvolume_;
  bool volume_ok_{false};
  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_{};
  BYTE version_major_{0};
  BYTE version_minor_{0};

  // MFT file records ($MFT file itself) may be fragmented
  // Get $MFT Data attribute to translate FileRecord to correct disk offset
  FileRecord mft_record_;              // $MFT File Record
  const AttrBase* mft_data_{nullptr};  // $MFT Data Attribute

  // Buffer of size file_record_size_ to read FileRecord.
  mutable std::vector<BYTE> file_record_buffer_;

  [[nodiscard]] bool OpenVolume(_TCHAR volume) noexcept;

 public:
  [[nodiscard]] bool IsVolumeOK() const noexcept;
  [[nodiscard]] std::pair<BYTE, BYTE> GetVersion() const noexcept;
  [[nodiscard]] ULONGLONG GetRecordsCount() const noexcept;

  [[nodiscard]] WORD GetSectorSize() const noexcept;
  [[nodiscard]] DWORD GetClusterSize() const noexcept;
  [[nodiscard]] DWORD GetFileRecordSize() const noexcept;
  [[nodiscard]] DWORD GetIndexBlockSize() const noexcept;
  [[nodiscard]] ULONGLONG GetMFTAddr() const noexcept;
  [[nodiscard]] BYTE* GetFileRecordBuffer() const noexcept;

  [[nodiscard]] bool InstallAttrRawCB(DWORD attrType,
                                      AttrRawCallback cb) noexcept;
  void AttrRawCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                       bool& bDiscard) const;
  void ClearAttrRawCB() noexcept;
};  // NtfsVolume
}  // namespace NtfsBrowser
