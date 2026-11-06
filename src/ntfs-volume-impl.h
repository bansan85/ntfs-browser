#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/efs/efs.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "alloc/cluster-map.h"
#include "attr/slot.h"
#include "mft/extent-map.h"
#include "mft/metadata-opener.h"
#include "mft/mft-bootstrap.h"
#include "volume/volume-geometry.h"

namespace NTFS_BROWSER_NAMESPACE {

class IDiskReader;

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io

namespace Usn {

struct Journal;

}  // namespace Usn
}  // namespace NTFS_BROWSER_NAMESPACE

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

  // The change journal ($UsnJrnl:$J of $Extend), loaded on first use and only
  // with recover_errors on. usn_journal stays null when the journal is absent
  // or unusable; usn_loaded then keeps the failure from being retried.
  mutable std::unique_ptr<Usn::Journal> usn_journal;

  // What the volume knows about its clusters, built on first use by
  // Clusters(). It reads through the geometry and the reader, which are final
  // by then.
  mutable std::unique_ptr<Alloc::ClusterMap<S>> clusters;

  // The EFS key source and cipher backend the caller chose, or the defaults.
  EfsSettings efs;

  mutable std::vector<BYTE> cluster_buffer;

  // Boot-sector geometry, as ParseBootSector() decoded it. Read by the
  // geometry getters, and by Init() and the cluster map.
  Volume::Geometry geometry;

  std::array<Attr::RawCallback, Attr::attr_nums> attr_raw_call_back{};
  bool volume_ok{false};
  BYTE version_major{0};
  BYTE version_minor{0};
  mutable bool upcase_loaded{false};
  mutable bool usn_loaded{false};
  // Set while the journal loads: a query made then sees no journal instead of
  // loading it again.
  mutable bool loading_usn{false};

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
  // VolumeOptions::media_kind when set, else what the reader says.
  [[nodiscard]] Tuning::MediaKind EffectiveMediaKind() const;
  void Init();
  [[nodiscard]] const UpCase::Table& GetUpCaseTable() const;
  // Opens the records of the metadata files. Nothing opens while the volume is
  // not OK.
  [[nodiscard]] Mft::MetadataOpener<S> MetadataFiles() const;
  // The volume's cluster map, built now if it was not.
  [[nodiscard]] Alloc::ClusterMap<S>& Clusters() const;
  // The disk ranges that a read of count records of $MFT from first_ref
  // reads, run by run. None where that read would fail at once: a record below
  // Mft::Idx::User, $MFT not mapped, nothing asked for, or an offset that
  // overflows.
  [[nodiscard]] std::vector<Mft::DiskRange>
      MftSlotRanges(ULONGLONG first_ref, ULONGLONG count) const;
  void AttrRawCallBack(DWORD att_type, const Attr::HeaderCommon& ahc,
                       bool& discard) const;
};

}  // namespace NTFS_BROWSER_NAMESPACE
