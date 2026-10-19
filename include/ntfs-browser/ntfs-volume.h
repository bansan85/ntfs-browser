#pragma once

#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#ifdef _WIN32
  #include <tchar.h>
#endif

namespace NTFS_BROWSER_NAMESPACE {

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace UpCase {

class Table;

}  // namespace UpCase

template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT NtfsVolume {
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
  NtfsVolume(const NtfsVolume& other) = delete;
  NtfsVolume& operator=(NtfsVolume&& other) noexcept = delete;
  NtfsVolume& operator=(const NtfsVolume& other) = delete;
  virtual ~NtfsVolume();

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

  // Reads length bytes at addr. FullCache returns a Cache::View: the bytes
  // stay valid while a copy of it lives and the volume is open. NoCache
  // returns a bare span, valid until the next read on this volume.
  [[nodiscard]] std::optional<Cache::ReadResult<S>> Read(LARGE_INTEGER& addr,
                                                         DWORD length) const;
  // Reads from addr into dest.
  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const;

  // Frees every cached block that no Cache::View uses. The volume stays open,
  // and a later read loads the data again. A no-op under NoCache.
  void MinifyCache() const;
  // The memory the cache holds right now. All zero under NoCache.
  [[nodiscard]] Cache::Stats GetCacheStats() const;

  [[nodiscard]] bool InstallAttrRawCB(Attr::Type attr_type,
                                      Attr::RawCallback callback) noexcept;
  void ClearAttrRawCB() noexcept;

  // Sets the source of the keys that decrypt EFS files. A null provider
  // disables decryption altogether, and the default provider with it.
  void SetEfsKeyProvider(
      std::shared_ptr<Efs::IEfsKeyProvider> provider) noexcept;

  // The installed provider. When none was ever installed, the first call
  // creates the default one: the current user's certificate store, on Windows.
  // Null where there is none, or after SetEfsKeyProvider(nullptr).
  [[nodiscard]] std::shared_ptr<Efs::IEfsKeyProvider> GetEfsKeyProvider() const;

  // Selects the cipher backend of this volume. Returns false, and keeps the
  // current one, where the backend is unavailable: BCrypt exists on Windows
  // only, and either backend can also be left out of the build entirely (see
  // the NTFS_BROWSER_ENABLE_EFS_CRYPTOPP / NTFS_BROWSER_ENABLE_EFS_BCRYPT
  // CMake options). When both are compiled in, BCrypt has no DESX, so a DESX
  // file falls back to CryptoPp regardless of which backend is selected;
  // with only BCrypt compiled in, a DESX file has no usable backend at all.
  // A file record parsed earlier keeps the backend it saw.
  [[nodiscard]] bool SetEfsCipherBackend(Efs::CipherBackend backend) noexcept;

  // The backend SetEfsCipherBackend() last accepted. Defaults to whichever
  // backend the build compiled in; CryptoPp if both are.
  [[nodiscard]] Efs::CipherBackend GetEfsCipherBackend() const noexcept;

  // Runs the raw attribute callback this volume has for att_type, if any.
  // att_type MUST be below Attr::attr_nums. The callback sets discard.
  void DispatchAttrRawCB(DWORD att_type, const Attr::HeaderCommon& ahc,
                         bool& discard) const;

  // True once the data extents of $MFT are mapped. Records from
  // Mft::Idx::User on are then read through them, not from a fixed address.
  [[nodiscard]] bool HasMftBase() const noexcept;
  // Reads out.size() bytes of $MFT's data at offset, through its own extents.
  // Returns the byte count read, or nothing when the read fails.
  [[nodiscard]] std::optional<ULONGLONG> ReadMftData(ULONGLONG offset,
                                                     std::span<BYTE> out) const;
  // Reads out.size() bytes of $MFT's data, from the slot of record first_ref.
  // False when the records below Mft::Idx::User are asked for (they are read
  // from a fixed address instead), when $MFT is not mapped, or on a short read.
  [[nodiscard]] bool ReadMftSlots(ULONGLONG first_ref,
                                  std::span<BYTE> out) const;
  // Reads out.size() bytes of the $MFTMirr copy of record file_ref. It reads
  // nothing and returns false unless recover_errors is on, the boot sector
  // names a usable $MFTMirr, and file_ref is below 4: the copy holds only
  // $MFT, $MFTMirr, $LogFile and $Volume. The copy may be older than $MFT's
  // own record.
  [[nodiscard]] bool ReadMftMirrorRecord(ULONGLONG file_ref,
                                         std::span<BYTE> out) const;

  // The volume's $UpCase table, loaded on first use. When it cannot be read,
  // the built-in Unicode simple uppercase mapping stands in.
  [[nodiscard]] const UpCase::Table& GetUpCaseTable() const;
};  // NtfsVolume

}  // namespace NTFS_BROWSER_NAMESPACE
