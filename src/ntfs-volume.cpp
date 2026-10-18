#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gsl/narrow>

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

#include "attr/non-resident.h"  // IWYU pragma: keep
#include "attr/resident.h"
#include "attr/slot.h"
#include "attr/vol-info.h"
#include "attr/vol-name.h"
#include "data/attribute-list.h"
#include "io/file-reader.h"  // IWYU pragma: keep
#include "io/file-record-impl.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "mft/record-offset.h"
#include "ntfs-volume-impl.h"
#include "upcase/upcase.h"
#include "utf/utf.h"
#include "volume/volume-geometry.h"

namespace NTFS_BROWSER_NAMESPACE {

class IDiskReader;

namespace Efs {

class IEfsKeyProvider;

}  // namespace Efs

namespace Attr {

struct HeaderCommon;

}  // namespace Attr
struct VolumeOptions;

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
  vol.impl_->bypass_deleted_gate = true;
  vol.SetAttrMask(Attr::Mask::VolumeName | Attr::Mask::VolumeInformation);
  if (!vol.ParseFileRecord(static_cast<DWORD>(Mft::Idx::Volume)) ||
      !vol.ParseAttrs()) {
    return false;
  }
  return !vol.GetAttr(Attr::Type::VolumeInformation).empty();
}

// Reads the NTFS version from $VOLUME_INFORMATION and logs it. False below 3.0,
// the NT4 era. Needs ParseVolumeRecord() to have passed first.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::AcceptVolumeVersion(const Io::FileRecord<S>& vol) {
  const auto* info =
      reinterpret_cast<const Attr::AttrVolInfo<Io::ResidentOf<S>, S>*>(
          vol.GetAttr(Attr::Type::VolumeInformation).front().get());
  std::tie(version_major, version_minor) = info->GetVersion();
  Log::Info("NTFS volume version: {}.{}", version_major, version_minor);
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

// Read the first sector (boot sector) and derive volume geometry from it
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::ParseBootSector() {
  // Held until the parse is done: under FullCache it owns the bytes.
  const std::optional<Cache::ReadResult<S>> bpb_buffer = ReadBootSector();
  if (!bpb_buffer) {
    Log::Error("Read boot sector error");
    return false;
  }
  const Volume::BootSectorParse parse = Volume::ParseBootSector(*bpb_buffer);
  geometry = parse.geometry;
  // Sized from the decoded cluster, even when a later field is rejected.
  cluster_buffer.resize(geometry.cluster_size);
  return parse.ok;
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
  record.impl_->bypass_deleted_gate = true;
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

template class NtfsVolume<Cache::Strategy::NoCache>;
template class NtfsVolume<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE
