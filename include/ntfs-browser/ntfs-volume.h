#pragma once

#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <ntfs-browser/data/attr-defines.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/efs.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#ifdef _WIN32
  #include <tchar.h>
#endif

namespace NtfsBrowser
{

template <Strategy S>
class FileRecord;

template <Strategy S>
class NTFS_BROWSER_EXPORT NtfsVolume
{
 public:
#ifdef _WIN32
  // Opens a real disk/device by drive letter, or an arbitrary device/image
  // path (eg. "\\\\.\\PhysicalDrive0", or a plain file for a disk image) via
  // Win32DiskReader. Not available outside Windows -- construct NtfsVolume
  // from an already-open IDiskReader there. options is fixed for the
  // volume's lifetime; read it back through GetOptions().
  explicit NtfsVolume(_TCHAR volume, const VolumeOptions& options = {});
  explicit NtfsVolume(std::wstring_view path,
                      const VolumeOptions& options = {});
#endif
  // Uses an already-open reader instead of opening a path (eg. an in-memory
  // or sequential test double, which have no real path to open).
  explicit NtfsVolume(std::unique_ptr<IDiskReader> reader,
                      const VolumeOptions& options = {});
  NtfsVolume(NtfsVolume&& other) noexcept = delete;
  NtfsVolume(NtfsVolume const& other) = delete;
  NtfsVolume& operator=(NtfsVolume&& other) noexcept = delete;
  NtfsVolume& operator=(NtfsVolume const& other) = delete;
  virtual ~NtfsVolume();

  friend class FileRecord<S>;

 private:
  // Every member and private method, kept out of this header.
  class Impl;
  std::unique_ptr<Impl> impl_;

 public:
  [[nodiscard]] bool IsVolumeOK() const noexcept;
  // The options this volume was constructed with.
  [[nodiscard]] const VolumeOptions& GetOptions() const noexcept;
  [[nodiscard]] std::pair<BYTE, BYTE> GetVersion() const noexcept;
  // Record slots $MFT holds: its declared size, capped at the clusters its
  // data runs map, since the size field is not validated on disk.
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
                                      AttrRawCallback callback) noexcept;
  void ClearAttrRawCB() noexcept;

  // Sets the source of the keys that decrypt EFS files. A null provider
  // disables decryption altogether, and the default provider with it.
  void SetEfsKeyProvider(
      std::shared_ptr<Efs::IEfsKeyProvider> provider) noexcept;

  // The installed provider. When none was ever installed, the first call
  // creates the default one: the current user's certificate store, on Windows.
  // Null where there is none, or after SetEfsKeyProvider(nullptr).
  [[nodiscard]] std::shared_ptr<Efs::IEfsKeyProvider> GetEfsKeyProvider() const;
};  // NtfsVolume
}  // namespace NtfsBrowser
