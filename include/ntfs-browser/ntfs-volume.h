#pragma once

#include <array>
#include <span>

#include <tchar.h>
#include <windows.h>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/file-reader.h>
#include <ntfs-browser/file-record.h>

namespace NtfsBrowser
{
class AttrBase;

class NtfsVolume
{
 public:
  explicit NtfsVolume(_TCHAR volume, FileReader::Strategy strategy);
  NtfsVolume(NtfsVolume&& other) noexcept = delete;
  NtfsVolume(NtfsVolume const& other) = delete;
  NtfsVolume& operator=(NtfsVolume&& other) noexcept = delete;
  NtfsVolume& operator=(NtfsVolume const& other) = delete;
  virtual ~NtfsVolume() = default;

  friend class FileRecord;
  friend class AttrBase;

 private:
  WORD sector_size_{0};
  DWORD cluster_size_{0};
  DWORD file_record_size_{0};
  DWORD index_block_size_{0};
  ULONGLONG mft_addr_{0};
  bool volume_ok_{false};
  std::array<AttrRawCallback, kAttrNums> attr_raw_call_back_{};
  BYTE version_major_{0};
  BYTE version_minor_{0};
  FileReader volume_;

  // MFT file records ($MFT file itself) may be fragmented
  // Get $MFT Data attribute to translate FileRecord to correct disk offset
  FileRecord mft_record_;              // $MFT File Record
  const AttrBase* mft_data_{nullptr};  // $MFT Data Attribute

  // Buffer of size file_record_size_ to read FileRecord.
  mutable std::vector<BYTE> cluster_buffer_;
  mutable std::vector<BYTE> file_record_buffer_;

  [[nodiscard]] bool OpenVolume(_TCHAR volume);

 public:
  [[nodiscard]] bool IsVolumeOK() const noexcept;
  [[nodiscard]] std::pair<BYTE, BYTE> GetVersion() const noexcept;
  [[nodiscard]] ULONGLONG GetRecordsCount() const noexcept;

  [[nodiscard]] WORD GetSectorSize() const noexcept;
  [[nodiscard]] DWORD GetClusterSize() const noexcept;
  [[nodiscard]] DWORD GetFileRecordSize() const noexcept;
  [[nodiscard]] DWORD GetIndexBlockSize() const noexcept;
  [[nodiscard]] ULONGLONG GetMFTAddr() const noexcept;

  [[nodiscard]] std::span<BYTE> GetClusterBuffer() const noexcept;
  [[nodiscard]] std::span<BYTE> GetFileRecordBuffer() const noexcept;

  [[nodiscard]] std::optional<std::span<const BYTE>> Read(LARGE_INTEGER& addr,
                                                          DWORD length) const;

  [[nodiscard]] bool InstallAttrRawCB(AttrType attrType,
                                      AttrRawCallback cb) noexcept;
  void AttrRawCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                       bool& bDiscard) const;
  void ClearAttrRawCB() noexcept;
};  // NtfsVolume
}  // namespace NtfsBrowser
