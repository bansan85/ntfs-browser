#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "alloc/bad-cluster-list.h"
#include "alloc/cluster-bitmap.h"
#include "attr/slot.h"
#include "mft/mft-bootstrap.h"
#include "volume/volume-geometry.h"

#ifdef _WIN32
  #include <tchar.h>
#endif

namespace NTFS_BROWSER_NAMESPACE {

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
// private methods that work on them. Only NtfsVolume<S> reaches it, through
// impl_: other classes use the methods of NtfsVolume<S> that forward to it.
template <Cache::Strategy S>
class NtfsVolume<S>::Impl {
 public:
  // EFS key source and cipher backend. A provider that was never chosen is
  // created on the first decryption, while a chosen null provider disables it.
  struct EfsSettings {
    std::shared_ptr<Efs::IEfsKeyProvider> provider;
    // False until SetEfsKeyProvider() or the first GetEfsKeyProvider() call.
    bool provider_set{false};

    // Defaults to whichever backend is compiled in, Crypto++ first.
#ifdef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
    Efs::CipherBackend backend{Efs::CipherBackend::CryptoPp};
#elif defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT)
    Efs::CipherBackend backend{Efs::CipherBackend::BCrypt};
#else
    Efs::CipherBackend backend{Efs::CipherBackend::CryptoPp};
#endif
  };

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

  // The bad cluster list ($Bad stream of $BadClus, MFT record 8), loaded on
  // first use. It is a copy, so no record stays alive for it. bad_list stays
  // null when $BadClus cannot be read; bad_loaded then keeps the failure from
  // being retried. Declared before the bitmap, which points to it, so it is
  // destroyed after it.
  mutable std::unique_ptr<Alloc::BadClusterList> bad_list;

  // The cluster bitmap ($Bitmap, MFT record 6), loaded on first use. The record
  // stays alive for as long as the bitmap reads through its data stream, so it
  // is declared first and destroyed last. bitmap stays null when $Bitmap cannot
  // be read; bitmap_loaded then keeps the failure from being retried.
  mutable std::unique_ptr<Io::FileRecord<S>> bitmap_record;
  mutable std::unique_ptr<Alloc::ClusterBitmap> bitmap;
  mutable std::optional<Alloc::SpaceSummary> summary;

  // The EFS key source and cipher backend the caller chose, or the defaults.
  EfsSettings efs;

  mutable std::vector<BYTE> cluster_buffer;

  // Boot-sector geometry, as ParseBootSector() decoded it. Read by the
  // geometry getters, and by Init() and the $UpCase load.
  Volume::Geometry geometry;

  std::array<Attr::RawCallback, Attr::attr_nums> attr_raw_call_back{};
  bool volume_ok{false};
  BYTE version_major{0};
  BYTE version_minor{0};
  mutable bool upcase_loaded{false};
  mutable bool bitmap_loaded{false};
  // Set while the bitmap loads: a query made then answers Unknown instead of
  // loading it again.
  mutable bool loading_bitmap{false};
  // Set once a partial or unusable bitmap was logged: that log is once per
  // volume.
  mutable bool degraded_logged{false};
  mutable bool summary_computed{false};
  mutable bool bad_loaded{false};
  // Set while the bad list loads: a query made then sees no list instead of
  // loading it again.
  mutable bool loading_bad{false};
  // Set once a bad cluster that the bitmap calls free was logged: that log is
  // once per volume.
  mutable bool bad_logged{false};

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
  [[nodiscard]] bool UseBackupBootSector();
  [[nodiscard]] std::optional<Cache::ReadResult<S>> ReadBootSector() const;
  [[nodiscard]] bool ParseVolumeRecord(Io::FileRecord<S>& vol);
  [[nodiscard]] bool AcceptVolumeVersion(const Io::FileRecord<S>& vol);
  void LogVolumeName(const Io::FileRecord<S>& vol) const;
  void Init();
  [[nodiscard]] const UpCase::Table& GetUpCaseTable() const;
  [[nodiscard]] std::unique_ptr<const UpCase::Table> LoadUpCaseTable() const;
  // The cluster bitmap, loaded now if it was not. Null when it is unusable, or
  // while it loads.
  [[nodiscard]] Alloc::ClusterBitmap* GetClusterBitmap() const;
  [[nodiscard]] std::unique_ptr<Alloc::ClusterBitmap> LoadClusterBitmap() const;
  // Logs, once per volume, that the bitmap turned out short or lost a window,
  // and, once per volume, that it calls free a cluster the bad list names.
  void NoteBitmapHealth() const;
  // The bad cluster list, loaded now if it was not. Null when it is unusable,
  // or while it loads.
  [[nodiscard]] const Alloc::BadClusterList* GetBadList() const;
  [[nodiscard]] std::unique_ptr<Alloc::BadClusterList>
      LoadBadClusterList() const;
  // The volume's cluster count, cut at the size of the backing store when that
  // is known. 0 when the boot sector gave none.
  [[nodiscard]] ULONGLONG UsableClusterCount() const;
  void AttrRawCallBack(DWORD att_type, const Attr::HeaderCommon& ahc,
                       bool& discard) const;
};

}  // namespace NTFS_BROWSER_NAMESPACE
