#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "attr/slot.h"
#include "mft/mft-bootstrap.h"
#include "volume/volume-geometry.h"

#ifdef _WIN32
  #include <tchar.h>
#endif

namespace NtfsBrowser {

// Caches reads from the volume's backing IDiskReader.
namespace Io {

template <Cache::Strategy S>
class FileReader;

}  // namespace Io

// The volume's $UpCase table.
namespace UpCase {

class Table;

}  // namespace UpCase

// Everything NtfsVolume<S> keeps out of its public header: the members, and the
// private methods that work on them. FileRecord<S>, a friend of NtfsVolume<S>,
// reaches it through NtfsVolume<S>::impl_.
template <Cache::Strategy S>
class NtfsVolume<S>::Impl {
 public:
  Impl(NtfsVolume<S>& self, const VolumeOptions& options);

  // The NtfsVolume this belongs to. The FileRecords it owns are built over it.
  NtfsVolume<S>* self;
  std::unique_ptr<Io::FileReader<S>> volume;

  // $MFT's own record and the extents that map it. Set up by Init(), once the
  // boot sector has fixed the cluster geometry.
  Mft::MftBootstrap<S> mft;

  // The volume's own $UpCase, loaded on first use by GetUpCaseTable(). Stays
  // null when $UpCase cannot be read; upcase_loaded_ then keeps the failure
  // from being retried.
  mutable std::unique_ptr<const UpCase::Table> upcase;

  // EFS key source. efs_provider_set_ tells "never chosen", which lets the
  // default provider be created on the first decryption, from "chosen to be
  // none" (SetEfsKeyProvider(nullptr)), which disables decryption.
  mutable std::shared_ptr<Efs::IEfsKeyProvider> efs_provider;

  mutable std::vector<BYTE> cluster_buffer;

  // Boot-sector geometry, as ParseBootSector() decoded it. Read by the
  // geometry getters, and by Init() and the $UpCase load.
  Volume::Geometry geometry;

  std::array<Attr::RawCallback, Attr::attr_nums> attr_raw_call_back{};
  bool volume_ok{false};
  BYTE version_major{0};
  BYTE version_minor{0};
  mutable bool efs_provider_set{false};

  // The selected EFS cipher backend. Defaults to whichever one is compiled
  // in, Crypto++ first.
#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
  Efs::CipherBackend efs_backend{Efs::CipherBackend::CryptoPp};
#elif defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
  Efs::CipherBackend efs_backend{Efs::CipherBackend::BCrypt};
#else
  Efs::CipherBackend efs_backend{Efs::CipherBackend::CryptoPp};
#endif
  mutable bool upcase_loaded{false};

  // Fixed for the volume's lifetime; set by the constructor, read back
  // through GetOptions(). No setter: every component that reads it goes
  // through this one volume-wide copy.
  VolumeOptions options;

#ifdef _WIN32
  [[nodiscard]] bool OpenVolume(_TCHAR volume);
  [[nodiscard]] bool OpenVolume(std::wstring_view path);
#endif
  [[nodiscard]] bool OpenVolume(std::unique_ptr<IDiskReader> reader);
  [[nodiscard]] bool ParseBootSector();
  [[nodiscard]] std::optional<std::span<const BYTE>> ReadBootSector() const;
  [[nodiscard]] bool ParseVolumeRecord(Io::FileRecord<S>& vol);
  [[nodiscard]] bool AcceptVolumeVersion(const Io::FileRecord<S>& vol);
  void LogVolumeName(const Io::FileRecord<S>& vol) const;
  void Init();
  [[nodiscard]] const UpCase::Table& GetUpCaseTable() const;
  [[nodiscard]] std::unique_ptr<const UpCase::Table> LoadUpCaseTable() const;
  void AttrRawCallBack(DWORD att_type, const Attr::HeaderCommon& ahc,
                       bool& discard) const;
};

}  // namespace NtfsBrowser
