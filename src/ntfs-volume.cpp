#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "alloc/bad-cluster-list.h"
#include "alloc/cluster-bitmap.h"
#include "attr/slot.h"
#include "attr/vol-info.h"
#include "attr/vol-name.h"
#include "data/volume-information.h"
#include "io/file-reader.h"
#include "io/file-record-attr-types.h"
#include "log/ntfs-common.h"
#include "mft/record-offset.h"
#include "ntfs-browser/efs/efs.h"
#include "ntfs-browser/volume-options.h"
#include "ntfs-volume-impl.h"
#include "upcase/upcase.h"
#include "usn/usn-journal-loader.h"
#include "utf/utf.h"
#include "volume/volume-geometry.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileReader;

}  // namespace Io

namespace Attr {

template <Cache::Strategy S>
class AttrBase;
template <typename Resident, Cache::Strategy S>
class AttrVolInfo;
template <typename Resident, Cache::Strategy S>
class AttrVolName;

}  // namespace Attr

namespace Usn {

struct Journal;

}  // namespace Usn
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

class IDiskReader;

namespace Attr {

struct HeaderCommon;

}  // namespace Attr

namespace {

// AttrVolName pads its buffer with a terminator that its view still
// covers. UTF-8 has no terminator convention, so the padding must go
// before converting, or it becomes a NUL byte inside the log line.
std::wstring_view TrimTrailingNuls(std::wstring_view name) noexcept {
  while (!name.empty() && name.back() == L'\0') {
    name.remove_suffix(1);
  }
  return name;
}

// Length of a Win32 volume device path, NUL excluded: \\.\C:
constexpr size_t volume_path_length = 6;

// Sector sizes the backup boot sector is tried with, most common first. The
// copy sits in the volume's last sector, whose size is not known yet.
constexpr std::array<WORD, 4> backup_sector_sizes{512, 4096, 1024, 2048};

// Bytes of a boot sector, whatever the sector size. It is also the smallest
// candidate sector size, so every backup read holds a whole boot sector.
constexpr WORD boot_sector_bytes = 512;

// Largest backup candidate read: the biggest sector size tried.
constexpr WORD backup_read_max_bytes = 4096;

// Reads out.size() bytes of a bitmap stream at offset. When the whole read
// fails or stops short, it reads again piece by piece, so the pieces before the
// damage are not lost with it. Returns the bytes read from the start, or
// nothing when not even the first piece could be read. Never throws.
template <Cache::Strategy S>
std::optional<ULONGLONG> ReadBitmapBytes(const Attr::AttrBase<S>& stream,
                                         DWORD piece, ULONGLONG offset,
                                         std::span<BYTE> out) {
  try {
    const std::optional<ULONGLONG> whole = stream.ReadData(offset, out);
    if (whole && *whole == out.size()) {
      return whole;
    }

    ULONGLONG done = 0;
    while (done < out.size()) {
      const size_t length =
          static_cast<size_t>(std::min<ULONGLONG>(piece, out.size() - done));
      const std::optional<ULONGLONG> part =
          stream.ReadData(offset + done, out.subspan(done, length));
      if (!part || *part == 0) {
        break;
      }
      done += *part;
      if (*part < length) {
        break;
      }
    }
    if (done == 0) {
      return std::nullopt;
    }
    return done;
  } catch (const std::exception& e) {
    Log::Exception(e);
    return std::nullopt;
  }
}

}  // namespace

template <Cache::Strategy S>
NtfsVolume<S>::Impl::Impl(NtfsVolume<S>& self, const VolumeOptions& options)
    : self(&self),
      volume(std::make_unique<Io::FileReader<S>>(options.max_cache_bytes)),
      mft(self),
      options(options) {}

#ifdef _WIN32
template <Cache::Strategy S>
NtfsVolume<S>::NtfsVolume(_TCHAR volume, const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options)) {
  if (impl_->OpenVolume(volume)) {
    impl_->Init();
  }
}

template <Cache::Strategy S>
NtfsVolume<S>::NtfsVolume(std::wstring_view path, const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options)) {
  if (impl_->OpenVolume(path)) {
    impl_->Init();
  }
}
#endif

template <Cache::Strategy S>
NtfsVolume<S>::NtfsVolume(std::unique_ptr<IDiskReader> reader,
                          const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options)) {
  if (impl_->OpenVolume(std::move(reader))) {
    impl_->Init();
  }
}

template <Cache::Strategy S>
NtfsVolume<S>::~NtfsVolume() = default;

// Parses $Volume's VolumeInformation and VolumeName. False when the record does
// not parse or has no VolumeInformation.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::ParseVolumeRecord(Io::FileRecord<S>& vol) {
  vol.SetBypassDeletedGate(true);
  vol.SetAttrMask(Attr::Mask::VolumeName | Attr::Mask::VolumeInformation);
  if (!vol.ParseFileRecord(static_cast<DWORD>(Mft::Idx::Volume)) ||
      !vol.ParseAttrs()) {
    return false;
  }
  return !vol.GetAttr(Attr::Type::VolumeInformation).empty();
}

// Reads the NTFS version from $VOLUME_INFORMATION and logs it, with a warning
// if the volume is dirty. False below 3.0, the NT4 era. Needs
// ParseVolumeRecord() to have passed first.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::AcceptVolumeVersion(const Io::FileRecord<S>& vol) {
  const auto* info =
      reinterpret_cast<const Attr::AttrVolInfo<Io::ResidentOf<S>, S>*>(
          vol.GetAttr(Attr::Type::VolumeInformation).front().get());
  std::tie(version_major, version_minor) = info->GetVersion();
  Log::Info("NTFS volume version: {}.{}", version_major, version_minor);
  if ((info->GetFlags() & Data::volume_dirty_flag) != 0) {
    Log::Warn(
        "NTFS volume was not unmounted cleanly: its journal was not "
        "replayed, so the data read may be stale");
  }
  return version_major >= 3;
}

// Logs $Volume's name, if it has a VolumeName attribute.
template <Cache::Strategy S>
void NtfsVolume<S>::Impl::LogVolumeName(const Io::FileRecord<S>& vol) const {
  const auto& names = vol.GetAttr(Attr::Type::VolumeName);
  if (names.empty()) {
    return;
  }
  const auto* name =
      reinterpret_cast<const Attr::AttrVolName<Io::ResidentOf<S>, S>*>(
          names.front().get());
  Log::Info("NTFS volume name: {}",
            Utf::WideToUtf8(TrimTrailingNuls(name->GetName())));
}

// Verify NTFS volume version (must >= 3.0) and locate $MFT's Data attribute
template <Cache::Strategy S>
void NtfsVolume<S>::Impl::Init() {
  Io::FileRecord<S> vol(*self);
  if (!ParseVolumeRecord(vol) || !AcceptVolumeVersion(vol)) {
    return;
  }
  LogVolumeName(vol);

  if (!mft.ParseRecord()) {
    return;
  }
  const Attr::AttrBase<S>* base_extent = mft.FindBaseExtent();
  if (base_extent == nullptr) {
    return;
  }

  // The cluster geometry is known only now, after the boot sector parse.
  mft.MapExtents(*base_extent, geometry.cluster_size,
                 geometry.file_record_size);

  // Reported OK only once the base extent is actually set.
  volume_ok = true;
}

#ifdef _WIN32
// Open a volume ('a' - 'z', 'A' - 'Z'), get volume handle and BPB
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(_TCHAR volume) {
  // Verify parameter
  if (!_istalpha(volume)) {
    Log::Error("Volume name error, should be like 'C', 'D'");
    return false;
  }

  std::array<_TCHAR, volume_path_length + 1> volume_path;
  _sntprintf_s(volume_path.data(), volume_path.size(), volume_path_length,
               _T("\\\\.\\%c:"), volume);
  std::get<volume_path_length>(volume_path) = _T('\0');

  return OpenVolume(std::wstring_view(volume_path.data()));
}

// Open an arbitrary device/image path, get volume handle and BPB
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(std::wstring_view path) {
  if (!volume->Open(path)) {
    Log::Error("Cannot open volume");
    return false;
  }

  if (options.read_ahead_bytes != 0) {
    volume->EnableReadAhead(options.read_ahead_bytes);
  }
  return ParseBootSector();
}
#endif

// Use an already-open reader (eg. a test double), get BPB
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(std::unique_ptr<IDiskReader> reader) {
  volume = std::make_unique<Io::FileReader<S>>(std::move(reader),
                                               options.max_cache_bytes);
  if (options.read_ahead_bytes != 0) {
    volume->EnableReadAhead(options.read_ahead_bytes);
  }

  return ParseBootSector();
}

// Read the first sector (boot sector) and derive volume geometry from it. When
// recovering and that sector is unusable, falls back on the backup boot sector.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::ParseBootSector() {
  bool accepted = false;
  {
    // Held until the parse is done: under FullCache it owns the bytes.
    const std::optional<Cache::ReadResult<S>> bpb_buffer = ReadBootSector();
    if (!bpb_buffer) {
      Log::Error("Read boot sector error");
    } else {
      const Volume::BootSectorParse parse =
          Volume::ParseBootSector(*bpb_buffer, options.recover_errors);
      geometry = parse.geometry;
      // Sized from the decoded cluster, even when a later field is rejected.
      cluster_buffer.resize(geometry.cluster_size);
      accepted = parse.ok;
    }
  }
  return accepted || (options.recover_errors && UseBackupBootSector());
}

// Looks for the copy of the boot sector that NTFS keeps in the last sector of
// the volume, and adopts its geometry when it parses. Only a volume that fills
// its whole backing store ends there: a partition larger than its file system
// puts the copy before the end. Each candidate is read apart from the block
// cache, so that FullCache does not ask for a 64 KiB block that runs past the
// end. False, with the geometry untouched, when no candidate fits.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::UseBackupBootSector() {
  const std::optional<ULONGLONG> volume_bytes = volume->Size();
  if (!volume_bytes || *volume_bytes < boot_sector_bytes) {
    Log::Info(
        "Cannot try the backup boot sector: the size of the volume is unknown "
        "or too small");
    return false;
  }

  for (const WORD sector_size : backup_sector_sizes) {
    if (TryBackupSectorSize(*volume_bytes, sector_size)) {
      return true;
    }
  }

  Log::Info("No usable backup boot sector");
  return false;
}

// Reads the last sector_size bytes of a backing store of volume_bytes, and
// adopts them as the boot sector when they parse with that same sector size.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::TryBackupSectorSize(ULONGLONG volume_bytes,
                                              WORD sector_size) {
  // The store MUST hold whole sectors, and the read MUST fit in it. The read
  // then starts on a sector boundary, which a raw device requires.
  if (volume_bytes % sector_size != 0 || volume_bytes < sector_size ||
      !std::in_range<LONGLONG>(volume_bytes - sector_size)) {
    return false;
  }

  LARGE_INTEGER addr{};
  addr.QuadPart = static_cast<LONGLONG>(volume_bytes - sector_size);
  std::array<BYTE, backup_read_max_bytes> copy{};
  const std::span<BYTE> buffer{copy.data(), sector_size};
  if (!volume->ReadInto(addr, buffer)) {
    return false;
  }

  const Volume::BootSectorParse parse = Volume::ParseBootSector(
      buffer.first<boot_sector_bytes>(), /*recover=*/true);
  // A copy that declares another sector size was not read at its own
  // boundary: it is garbage for this candidate.
  if (!parse.ok || parse.geometry.sector_size != sector_size) {
    return false;
  }
  geometry = parse.geometry;
  cluster_buffer.resize(geometry.cluster_size);
  Log::Info("Boot sector is unusable: using the backup boot sector at 0x{:X}",
            addr.QuadPart);
  return true;
}

// Read the boot sector, retrying with 512 bytes when the 4096-byte read fails.
template <Cache::Strategy S>
std::optional<Cache::ReadResult<S>>
    NtfsVolume<S>::Impl::ReadBootSector() const {
  // Smallest sector size NTFS uses. It is also the size of the BPB read.
  constexpr DWORD min_sector_size = 512;
  // Largest sector size NTFS supports (4Kn). Windows opens a volume handle
  // unbuffered, and then only accepts a read length that is a whole number
  // of the device's sectors.
  constexpr DWORD max_sector_size = 4096;
  LARGE_INTEGER fr_addr{.QuadPart = 0};
  std::optional<Cache::ReadResult<S>> bpb_buffer =
      volume->Read(fr_addr, max_sector_size);
  if (!bpb_buffer) {
    // A backing file shorter than max_sector_size cannot serve that read.
    Log::Warn("Cannot read a {}-byte boot sector, retrying with {} bytes",
              max_sector_size, min_sector_size);
    fr_addr.QuadPart = 0;
    bpb_buffer = volume->Read(fr_addr, min_sector_size);
  }
  return bpb_buffer;
}

// Check if Volume is successfully opened
template <Cache::Strategy S>
bool NtfsVolume<S>::IsVolumeOK() const noexcept {
  return impl_->volume_ok;
}

template <Cache::Strategy S>
const VolumeOptions& NtfsVolume<S>::GetOptions() const noexcept {
  return impl_->options;
}

// Get NTFS volume version
template <Cache::Strategy S>
std::pair<BYTE, BYTE> NtfsVolume<S>::GetVersion() const noexcept {
  return {impl_->version_major, impl_->version_minor};
}

// Get File Record count
template <Cache::Strategy S>
ULONGLONG NtfsVolume<S>::GetRecordsCount() const noexcept {
  return impl_->mft.Map().RecordCount();
}

// Get BPB information
template <Cache::Strategy S>
WORD NtfsVolume<S>::GetSectorSize() const noexcept {
  return impl_->geometry.sector_size;
}

template <Cache::Strategy S>
DWORD NtfsVolume<S>::GetClusterSize() const noexcept {
  return impl_->geometry.cluster_size;
}

template <Cache::Strategy S>
DWORD NtfsVolume<S>::GetFileRecordSize() const noexcept {
  return impl_->geometry.file_record_size;
}

template <Cache::Strategy S>
DWORD NtfsVolume<S>::GetIndexBlockSize() const noexcept {
  return impl_->geometry.index_block_size;
}

// Get MFT starting address
template <Cache::Strategy S>
ULONGLONG NtfsVolume<S>::GetMFTAddr() const noexcept {
  return impl_->geometry.mft_addr;
}

template <Cache::Strategy S>
std::span<BYTE> NtfsVolume<S>::GetClusterBuffer() const noexcept {
  return {impl_->cluster_buffer.data(), impl_->cluster_buffer.size()};
}

template <Cache::Strategy S>
std::optional<Cache::ReadResult<S>> NtfsVolume<S>::Read(LARGE_INTEGER& addr,
                                                        DWORD length) const {
  return impl_->volume->Read(addr, length);
}

template <Cache::Strategy S>
void NtfsVolume<S>::MinifyCache() const {
  impl_->volume->Minify();
}

template <Cache::Strategy S>
Cache::Stats NtfsVolume<S>::GetCacheStats() const {
  return impl_->volume->GetCacheStats();
}

template <Cache::Strategy S>
bool NtfsVolume<S>::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const {
  return impl_->volume->ReadInto(addr, dest);
}

template <Cache::Strategy S>
void NtfsVolume<S>::Prefetch(ULONGLONG addr, ULONGLONG length) const {
  impl_->volume->Prefetch(addr, length);
}

template <Cache::Strategy S>
void NtfsVolume<S>::CancelPrefetch() const {
  impl_->volume->CancelPrefetch();
}

// Install Attribute CallBack routines for the whole Volume
template <Cache::Strategy S>
bool NtfsVolume<S>::InstallAttrRawCB(Attr::Type attr_type,
                                     Attr::RawCallback callback) noexcept {
  const DWORD at_idx = Attr::AttrIndex(attr_type);
  if (at_idx >= Attr::attr_nums) {
    return false;
  }

  // atIdx < attr_nums was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  impl_->attr_raw_call_back[at_idx] = callback;
  return true;
}

// attType is an already-bounds-checked index into attr_raw_call_back_
// (attr_nums), not a raw Attr::Type/DWORD value. Only FileRecord<S> calls it,
// through NtfsVolume<S>::DispatchAttrRawCB(), from an index it already
// validated, which avoids exposing an unbounded array index through the public
// API. See N11 in docs/bug-reports/2026-09-03-full-repo.md.
template <Cache::Strategy S>
void NtfsVolume<S>::Impl::AttrRawCallBack(DWORD att_type,
                                          const Attr::HeaderCommon& ahc,
                                          bool& discard) const {
  // The caller passes an index below attr_nums, as described above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  if (attr_raw_call_back[att_type] != nullptr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    attr_raw_call_back[att_type](ahc, discard);
  }
}

template <Cache::Strategy S>
void NtfsVolume<S>::DispatchAttrRawCB(DWORD att_type,
                                      const Attr::HeaderCommon& ahc,
                                      bool& discard) const {
  impl_->AttrRawCallBack(att_type, ahc, discard);
}

template <Cache::Strategy S>
bool NtfsVolume<S>::HasMftBase() const noexcept {
  return impl_->mft.Map().HasBase();
}

template <Cache::Strategy S>
std::optional<ULONGLONG> NtfsVolume<S>::ReadMftData(ULONGLONG offset,
                                                    std::span<BYTE> out) const {
  return impl_->mft.Map().ReadData(offset, out);
}

template <Cache::Strategy S>
bool NtfsVolume<S>::ReadMftSlots(ULONGLONG first_ref,
                                 std::span<BYTE> out) const {
  if (first_ref < static_cast<ULONGLONG>(Mft::Idx::User) || !HasMftBase()) {
    return false;
  }

  const std::optional<ULONGLONG> offset =
      Mft::RecordOffset(GetFileRecordSize(), first_ref);
  if (!offset) {
    return false;
  }

  const std::optional<ULONGLONG> len = ReadMftData(*offset, out);
  return len && *len == out.size();
}

template <Cache::Strategy S>
void NtfsVolume<S>::PrefetchMftSlots(ULONGLONG first_ref,
                                     ULONGLONG count) const {
  if (first_ref < static_cast<ULONGLONG>(Mft::Idx::User) || !HasMftBase() ||
      count == 0) {
    return;
  }

  const std::optional<ULONGLONG> offset =
      Mft::RecordOffset(GetFileRecordSize(), first_ref);
  const ULONGLONG record_size = GetFileRecordSize();
  if (!offset || record_size == 0 ||
      count > std::numeric_limits<ULONGLONG>::max() / record_size) {
    return;
  }
  impl_->mft.Map().PrefetchData(*offset, count * record_size);
}

template <Cache::Strategy S>
bool NtfsVolume<S>::ReadMftMirrorRecord(ULONGLONG file_ref,
                                        std::span<BYTE> out) const {
  const Volume::Geometry& geometry = impl_->geometry;
  if (!impl_->options.recover_errors || !geometry.mft_mirr_usable ||
      file_ref >= Volume::mft_mirror_record_count) {
    return false;
  }

  // The geometry check bounds the copy, so this sum fits a LONGLONG.
  LARGE_INTEGER addr{};
  addr.QuadPart = static_cast<LONGLONG>(geometry.mft_mirr_addr +
                                        file_ref * geometry.file_record_size);
  return impl_->volume->ReadInto(addr, out);
}

template <Cache::Strategy S>
const UpCase::Table& NtfsVolume<S>::GetUpCaseTable() const {
  return impl_->GetUpCaseTable();
}

// Clear all Attribute CallBack routines
template <Cache::Strategy S>
void NtfsVolume<S>::ClearAttrRawCB() noexcept {
  for (Attr::RawCallback& call_back : impl_->attr_raw_call_back) {
    call_back = nullptr;
  }
}

template <Cache::Strategy S>
bool NtfsVolume<S>::HasAttrRawCB() const noexcept {
  return std::ranges::any_of(
      impl_->attr_raw_call_back,
      [](const Attr::RawCallback& call_back) { return call_back != nullptr; });
}

template <Cache::Strategy S>
void NtfsVolume<S>::SetEfsKeyProvider(
    std::shared_ptr<Efs::IEfsKeyProvider> provider) noexcept {
  impl_->efs.provider = std::move(provider);
  impl_->efs.provider_set = true;
}

template <Cache::Strategy S>
std::shared_ptr<Efs::IEfsKeyProvider> NtfsVolume<S>::GetEfsKeyProvider() const {
  if (!impl_->efs.provider_set) {
    impl_->efs.provider_set = true;
#ifdef _WIN32
    impl_->efs.provider = Efs::MakeCertStoreKeyProvider();
#endif
  }
  return impl_->efs.provider;
}

template <Cache::Strategy S>
bool NtfsVolume<S>::SetEfsCipherBackend(Efs::CipherBackend backend) noexcept {
  if (backend == Efs::CipherBackend::CryptoPp) {
#ifndef NTFS_BROWSER_ENABLE_EFS_CRYPTOPP
    return false;
#endif
  } else if (backend == Efs::CipherBackend::BCrypt) {
#if !(defined(_WIN32) && defined(NTFS_BROWSER_ENABLE_EFS_BCRYPT))
    return false;
#endif
  }
  impl_->efs.backend = backend;
  return true;
}

template <Cache::Strategy S>
Efs::CipherBackend NtfsVolume<S>::GetEfsCipherBackend() const noexcept {
  return impl_->efs.backend;
}

// Reads $UpCase (MFT record 10). Null when it is missing or unusable.
template <Cache::Strategy S>
std::unique_ptr<const UpCase::Table>
    NtfsVolume<S>::Impl::LoadUpCaseTable() const {
  const auto upcase_record = static_cast<ULONGLONG>(Mft::Idx::UpCase);
  const DWORD record_size = geometry.file_record_size;
  if (!volume_ok ||
      !mft.Map().IsRangeMapped(upcase_record * record_size, record_size)) {
    return {};
  }

  // Like the volume's other metadata reads, it MUST NOT depend on
  // include_deleted.
  Io::FileRecord<S> record(*self);
  record.SetBypassDeletedGate(true);
  record.SetAttrMask(Attr::Mask::Data);
  if (!record.ParseFileRecord(upcase_record) || !record.ParseAttrs()) {
    return {};
  }

  const Attr::AttrBase<S>* data = record.FindStream({});
  if (data == nullptr || data->GetDataSize() < UpCase::Table::byte_count) {
    return {};
  }

  std::vector<BYTE> bytes(UpCase::Table::byte_count);
  const std::optional<ULONGLONG> len = data->ReadData(0, bytes);
  if (!len || *len != bytes.size()) {
    return {};
  }

  std::optional<UpCase::Table> table = UpCase::Table::FromBytes(bytes);
  if (!table) {
    return {};
  }
  return std::make_unique<const UpCase::Table>(std::move(*table));
}

// Loads $UpCase on first use. A failure is cached: the built-in mapping
// answers every later call.
template <Cache::Strategy S>
const UpCase::Table& NtfsVolume<S>::Impl::GetUpCaseTable() const {
  if (!upcase_loaded) {
    upcase_loaded = true;
    upcase = LoadUpCaseTable();
    if (!upcase) {
      Log::Info("$UpCase is not usable: names collate by the built-in mapping");
    }
  }
  return upcase ? *upcase : UpCase::Table::BuiltIn();
}

// Reads $Bitmap (MFT record 6) and wraps its data stream. Null when the record
// or the stream is missing or unusable. Like the volume's other metadata reads,
// it MUST NOT depend on include_deleted. With recover_errors off a damaged run
// list rejects the attribute, so the bitmap is absent; on, the runs that parse
// are kept, and the clusters past them read as unknown.
template <Cache::Strategy S>
std::unique_ptr<Alloc::ClusterBitmap>
    NtfsVolume<S>::Impl::LoadClusterBitmap() const {
  const auto bitmap_record_idx = static_cast<ULONGLONG>(Mft::Idx::Bitmap);
  const DWORD record_size = geometry.file_record_size;
  if (!volume_ok ||
      !mft.Map().IsRangeMapped(bitmap_record_idx * record_size, record_size)) {
    return {};
  }

  auto record = std::make_unique<Io::FileRecord<S>>(*self);
  record->SetBypassDeletedGate(true);
  record->SetAttrMask(Attr::Mask::Data);
  if (!record->ParseFileRecord(bitmap_record_idx) || !record->ParseAttrs()) {
    return {};
  }

  const Attr::AttrBase<S>* data = record->FindStream({});
  if (data == nullptr) {
    return {};
  }

  // The store cannot hold more clusters than its size says, and a volume that
  // claims more is cut at the store. Bytes of bitmap past what those clusters
  // need are never looked at, so a forged size cannot make a query long.
  ULONGLONG cluster_count = geometry.cluster_count;
  ULONGLONG data_size = data->GetDataSize();
  if (const std::optional<ULONGLONG> store_bytes = volume->Size();
      store_bytes && geometry.cluster_size != 0) {
    const ULONGLONG store_clusters = *store_bytes / geometry.cluster_size;
    cluster_count =
        cluster_count == 0 ? 0 : std::min(cluster_count, store_clusters);
    data_size = std::min(data_size, store_clusters / 8 + 1);
  }
  if (cluster_count != 0) {
    data_size = std::min(data_size, cluster_count / 8 + 1);
  }

  const DWORD piece = geometry.cluster_size;
  auto reader = [data, piece](ULONGLONG offset, std::span<BYTE> out) {
    return ReadBitmapBytes<S>(*data, piece, offset, out);
  };
  auto loaded = std::make_unique<Alloc::ClusterBitmap>(
      reader, data_size, cluster_count, Alloc::BitmapWindowBytes());
  bitmap_record = std::move(record);
  return loaded;
}

// The volume's cluster count, cut where the backing store ends: a volume that
// claims more clusters than its store can hold is cut at the store.
template <Cache::Strategy S>
ULONGLONG NtfsVolume<S>::Impl::UsableClusterCount() const {
  ULONGLONG cluster_count = geometry.cluster_count;
  if (const std::optional<ULONGLONG> store_bytes = volume->Size();
      store_bytes && geometry.cluster_size != 0 && cluster_count != 0) {
    cluster_count =
        std::min(cluster_count, *store_bytes / geometry.cluster_size);
  }
  return cluster_count;
}

// Reads $BadClus (MFT record 8) and copies the real runs of its $Bad stream.
// Null when the record or the stream is missing or unusable, or when the runs
// are damaged and recover_errors is off. Like the volume's other metadata
// reads, it MUST NOT depend on include_deleted. The record is local: once the
// extents are copied, its run list is released.
template <Cache::Strategy S>
std::unique_ptr<Alloc::BadClusterList>
    NtfsVolume<S>::Impl::LoadBadClusterList() const {
  const auto bad_record_idx = static_cast<ULONGLONG>(Mft::Idx::BadCluster);
  const DWORD record_size = geometry.file_record_size;
  if (!volume_ok ||
      !mft.Map().IsRangeMapped(bad_record_idx * record_size, record_size)) {
    return {};
  }

  Io::FileRecord<S> record(*self);
  record.SetBypassDeletedGate(true);
  record.SetAttrMask(Attr::Mask::Data);
  if (!record.ParseFileRecord(bad_record_idx) || !record.ParseAttrs()) {
    return {};
  }

  const Attr::AttrBase<S>* stream = record.FindStream(L"$Bad");
  if (stream == nullptr) {
    return {};
  }
  return Alloc::BadClusterList::Build(
      stream->GetExtents(), UsableClusterCount(), options.recover_errors);
}

// Loads the bad cluster list on first use. A failure is cached: every later
// query goes on without the list, and the bitmap alone answers.
template <Cache::Strategy S>
const Alloc::BadClusterList* NtfsVolume<S>::Impl::GetBadList() const {
  if (loading_bad) {
    return nullptr;
  }
  if (!bad_loaded) {
    bad_loaded = true;
    loading_bad = true;
    try {
      bad_list = LoadBadClusterList();
    } catch (const std::exception& e) {
      Log::Exception(e);
      bad_list.reset();
    }
    loading_bad = false;
    if (!bad_list) {
      Log::Recoverable(options.recover_errors,
                       "$BadClus is not usable: bad clusters are unknown");
    }
  }
  return bad_list.get();
}

// Loads the cluster bitmap on first use, and the bad list with it. A failure is
// cached: every later query answers unknown without trying again. When only the
// bitmap fails, a bitmap with no bits stands in, so the clusters the bad list
// names stay bad and the rest are unknown.
template <Cache::Strategy S>
Alloc::ClusterBitmap* NtfsVolume<S>::Impl::GetClusterBitmap() const {
  if (loading_bitmap) {
    return nullptr;
  }
  if (!bitmap_loaded) {
    bitmap_loaded = true;
    const Alloc::BadClusterList* bad = GetBadList();
    loading_bitmap = true;
    try {
      bitmap = LoadClusterBitmap();
    } catch (const std::exception& e) {
      Log::Exception(e);
      bitmap.reset();
    }
    loading_bitmap = false;
    if (!bitmap) {
      degraded_logged = true;
      Log::Recoverable(options.recover_errors,
                       "$Bitmap is not usable: cluster allocation is unknown");
      const ULONGLONG cluster_count = UsableClusterCount();
      if (bad != nullptr && !bad->Get().extents.empty() && cluster_count != 0) {
        bitmap = std::make_unique<Alloc::ClusterBitmap>(
            [](ULONGLONG, std::span<BYTE>) -> std::optional<ULONGLONG> {
              return std::nullopt;
            },
            0, cluster_count, Alloc::BitmapWindowBytes());
      }
    }
    if (bitmap) {
      bitmap->SetBadList(bad);
      bitmap->SetRetention(KeptBitmapBytes());
    }
  }
  return bitmap.get();
}

template <Cache::Strategy S>
void NtfsVolume<S>::Impl::NoteBitmapHealth() const {
  if (bitmap && !bad_logged && bitmap->GetStatus().bad_with_clear_bit) {
    bad_logged = true;
    Log::Recoverable(options.recover_errors,
                     "Clusters listed in $BadClus are free in $Bitmap: they "
                     "are bad all the same");
  }
  if (!bitmap || degraded_logged) {
    return;
  }
  const Alloc::ClusterBitmap::Status status = bitmap->GetStatus();
  if (status.shorter_than_volume || status.failed_windows != 0) {
    degraded_logged = true;
    Log::Recoverable(options.recover_errors,
                     "$Bitmap is {}: some clusters are unknown",
                     status.shorter_than_volume ? "shorter than the volume"
                                                : "partly unreadable");
  }
}

template <Cache::Strategy S>
Alloc::ClusterState NtfsVolume<S>::GetClusterState(ULONGLONG lcn) const {
  return GetRangeState(lcn, 1);
}

template <Cache::Strategy S>
Alloc::ClusterState NtfsVolume<S>::GetRangeState(ULONGLONG lcn,
                                                 ULONGLONG clusters) const {
  Alloc::RangeQuery query{.lcn = lcn, .clusters = clusters};
  GetRangeStates({&query, 1});
  return query.state;
}

template <Cache::Strategy S>
void NtfsVolume<S>::GetRangeStates(std::span<Alloc::RangeQuery> queries) const {
  // A query nobody answers stays unknown.
  for (Alloc::RangeQuery& query : queries) {
    query.state = Alloc::ClusterState::Unknown;
    query.allocated_clusters = 0;
    query.free_clusters = 0;
    query.bad_clusters = 0;
  }

  Alloc::ClusterBitmap* bitmap = impl_->GetClusterBitmap();
  if (bitmap == nullptr) {
    return;
  }
  try {
    bitmap->StatesInOrder(queries);
  } catch (const std::exception& e) {
    Log::Exception(e);
  }
  impl_->NoteBitmapHealth();
}

template <Cache::Strategy S>
void NtfsVolume<S>::GetClusterRuns(ULONGLONG lcn, ULONGLONG clusters,
                                   std::vector<Alloc::ClusterRun>& runs) const {
  runs.clear();
  if (clusters == 0) {
    return;
  }
  Alloc::ClusterBitmap* bitmap = impl_->GetClusterBitmap();
  if (bitmap != nullptr) {
    try {
      bitmap->StateRuns(lcn, clusters, runs);
      impl_->NoteBitmapHealth();
      return;
    } catch (const std::exception& e) {
      Log::Exception(e);
    }
  }
  // A query nobody answers stays unknown.
  runs.assign(1, {.lcn = lcn,
                  .clusters = clusters,
                  .state = Alloc::ClusterState::Unknown});
}

template <Cache::Strategy S>
size_t NtfsVolume<S>::Impl::KeptBitmapBytes() const noexcept {
  return static_cast<size_t>(
      (std::min<ULONGLONG>)(bitmap_keep_bytes,
                            (std::numeric_limits<size_t>::max)()));
}

template <Cache::Strategy S>
void NtfsVolume<S>::KeepBitmapWindows(ULONGLONG max_bytes) const {
  impl_->bitmap_keep_bytes = max_bytes;
  if (impl_->bitmap) {
    impl_->bitmap->SetRetention(impl_->KeptBitmapBytes());
  }
}

template <Cache::Strategy S>
std::optional<Alloc::SpaceSummary> NtfsVolume<S>::GetSpaceSummary() const {
  if (!impl_->summary_computed) {
    impl_->summary_computed = true;
    Alloc::ClusterBitmap* bitmap = impl_->GetClusterBitmap();
    if (bitmap != nullptr) {
      try {
        impl_->summary = bitmap->Summary(impl_->geometry.cluster_size);
      } catch (const std::exception& e) {
        Log::Exception(e);
      }
      impl_->NoteBitmapHealth();
    }
  }
  return impl_->summary;
}

template <Cache::Strategy S>
const Alloc::BadClusters* NtfsVolume<S>::GetBadClusters() const {
  try {
    const Alloc::BadClusterList* list = impl_->GetBadList();
    return list != nullptr ? &list->Get() : nullptr;
  } catch (const std::exception& e) {
    Log::Exception(e);
    return nullptr;
  }
}

// Loads the change journal on first use. Strict mode returns null before any
// read. A failure is cached: every later call answers null without trying
// again, and the first one is logged once.
template <Cache::Strategy S>
const Usn::Journal* NtfsVolume<S>::GetUsnJournal() const {
  if (!impl_->options.recover_errors || impl_->loading_usn) {
    return nullptr;
  }
  if (!impl_->usn_loaded) {
    impl_->usn_loaded = true;
    impl_->loading_usn = true;
    impl_->usn_journal = Usn::LoadJournal(*this);
    impl_->loading_usn = false;
    if (!impl_->usn_journal) {
      Log::Recoverable(impl_->options.recover_errors,
                       "$UsnJrnl is not usable: the change journal is unknown");
    } else {
      if (impl_->usn_journal->damaged_records != 0) {
        Log::Recoverable(
            impl_->options.recover_errors,
            "$UsnJrnl: {} damaged stretch(es) of the journal were skipped",
            impl_->usn_journal->damaged_records);
      }
      if (impl_->usn_journal->truncated) {
        Log::Recoverable(
            impl_->options.recover_errors,
            "$UsnJrnl: the journal holds more than {} records, the oldest "
            "were dropped",
            impl_->options.max_usn_records);
      }
    }
  }
  return impl_->usn_journal.get();
}

template class NtfsVolume<Cache::Strategy::NoCache>;
template class NtfsVolume<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE
