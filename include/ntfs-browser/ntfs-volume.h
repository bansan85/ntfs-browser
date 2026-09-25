#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/efs.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/file-reader.h>
#include <ntfs-browser/file-record.h>

#ifdef _WIN32
  #include <tchar.h>
#endif

namespace NtfsBrowser
{

template <Strategy S>
class NTFS_BROWSER_EXPORT NtfsVolume
{
 public:
#ifdef _WIN32
  // Opens a real disk/device by drive letter, or an arbitrary device/image
  // path (eg. "\\\\.\\PhysicalDrive0", or a plain file for a disk image) via
  // Win32DiskReader. Not available outside Windows -- construct NtfsVolume
  // from an already-open IDiskReader there.
  explicit NtfsVolume(_TCHAR volume);
  explicit NtfsVolume(std::wstring_view path);
#endif
  // Uses an already-open reader instead of opening a path (eg. an in-memory
  // or sequential test double, which have no real path to open).
  explicit NtfsVolume(std::unique_ptr<IDiskReader> reader);
  NtfsVolume(NtfsVolume&& other) noexcept = delete;
  NtfsVolume(NtfsVolume const& other) = delete;
  NtfsVolume& operator=(NtfsVolume&& other) noexcept = delete;
  NtfsVolume& operator=(NtfsVolume const& other) = delete;
  virtual ~NtfsVolume() = default;

  friend class FileRecord<S>;
  friend class AttrBase<S>;

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
  FileReader<S> volume_;

  // MFT file records ($MFT file itself) may be fragmented
  // Get $MFT Data attribute to translate FileRecord to correct disk offset
  FileRecord<S> mft_record_;              // $MFT File Record
  const AttrBase<S>* mft_data_{nullptr};  // $MFT Data Attribute

  // Every $MFT DATA attribute instance: the base one, plus any continuation
  // reached through $MFT's own $ATTRIBUTE_LIST when its data runs don't fit
  // in one instance. ReadMftData() picks whichever instance covers the VCN
  // it needs.
  std::vector<const AttrBase<S>*> mft_data_instances_;

  mutable std::vector<BYTE> cluster_buffer_;

  // EFS key source. efs_provider_set_ tells "never chosen", which lets the
  // default provider be created on the first decryption, from "chosen to be
  // none" (SetEfsKeyProvider(nullptr)), which disables decryption.
  mutable std::shared_ptr<Efs::IEfsKeyProvider> efs_provider_;
  mutable bool efs_provider_set_{false};

#ifdef _WIN32
  [[nodiscard]] bool OpenVolume(_TCHAR volume);
  [[nodiscard]] bool OpenVolume(std::wstring_view path);
#endif
  [[nodiscard]] bool OpenVolume(std::unique_ptr<IDiskReader> reader);
  [[nodiscard]] bool ParseBootSector();
  void Init();
  [[nodiscard]] const AttrBase<S>* FindMftDataInstance(ULONGLONG vcn) const;
  [[nodiscard]] std::optional<ULONGLONG>
      ReadMftData(ULONGLONG offset, std::span<BYTE> buffer) const;

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

  [[nodiscard]] std::optional<std::span<const BYTE>> Read(LARGE_INTEGER& addr,
                                                          DWORD length) const;
  // Reads from addr into dest.
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const;

  [[nodiscard]] bool InstallAttrRawCB(AttrType attrType,
                                      AttrRawCallback cb) noexcept;
  void ClearAttrRawCB() noexcept;

  // Sets the source of the keys that decrypt EFS files. A null provider
  // disables decryption altogether, and the default provider with it.
  void SetEfsKeyProvider(
      std::shared_ptr<Efs::IEfsKeyProvider> provider) noexcept;

  // The installed provider. When none was ever installed, the first call
  // creates the default one: the current user's certificate store, on Windows.
  // Null where there is none, or after SetEfsKeyProvider(nullptr).
  [[nodiscard]] std::shared_ptr<Efs::IEfsKeyProvider> GetEfsKeyProvider() const;

 private:
  // attType is an already-bounds-checked index into attr_raw_call_back_
  // (kAttrNums), not a raw AttrType/DWORD value - keeping this private (only
  // FileRecord<S>, a friend, calls it, from an index it already validated)
  // avoids exposing an unbounded array index through the public API. See
  // N11 in docs/bug-reports/2026-09-03-full-repo.md.
  void AttrRawCallBack(DWORD attType, const AttrHeaderCommon& ahc,
                       bool& bDiscard) const;
};  // NtfsVolume
}  // namespace NtfsBrowser
