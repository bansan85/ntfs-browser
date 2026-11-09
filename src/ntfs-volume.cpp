#include "ntfs-volume.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/alloc-loader.h"
#include "bench/bench.h"
#include "bench/recorder.h"
#include "io/file-reader.h"
#include "log/ntfs-common.h"
#include "mft/idx.h"
#include "mft/record-offset.h"
#include "ntfs-volume-impl.h"
#include "tuning/tuning.h"
#include "upcase/upcase-loader.h"
#include "upcase/upcase.h"
#include "usn/usn-journal-loader.h"
#include "volume/boot-sector-loader.h"
#include "volume/volume-record-loader.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace {

// Length of a Win32 volume device path, NUL excluded: \\.\C:
constexpr size_t volume_path_length = 6;

}  // namespace

template <Cache::Strategy S>
NtfsVolume<S>::Impl::Impl(NtfsVolume<S>& self, const VolumeOptions& options)
    : self(&self),
      volume(std::make_unique<Io::FileReader<S>>(options.max_cache_bytes,
                                                 options.tuning)),
      mft(self),
      options(options) {}

#ifdef _WIN32
template <Cache::Strategy S>
NtfsVolume<S>::NtfsVolume(_TCHAR volume, const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options)) {
  const Bench::PhaseScope phase(Bench::Phase::Open);
  if (impl_->OpenVolume(volume)) {
    impl_->Init();
  }
}

template <Cache::Strategy S>
NtfsVolume<S>::NtfsVolume(std::wstring_view path, const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options)) {
  const Bench::PhaseScope phase(Bench::Phase::Open);
  if (impl_->OpenVolume(path)) {
    impl_->Init();
  }
}
#endif

template <Cache::Strategy S>
NtfsVolume<S>::NtfsVolume(std::unique_ptr<IDiskReader> reader,
                          const VolumeOptions& options)
    : impl_(std::make_unique<Impl>(*this, options)) {
  const Bench::PhaseScope phase(Bench::Phase::Open);
  if (impl_->OpenVolume(std::move(reader))) {
    impl_->Init();
  }
}

template <Cache::Strategy S>
NtfsVolume<S>::~NtfsVolume() = default;

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
  // A caller that states the media kind has no use for the host volume's.
  if (!volume->Open(path, !options.media_kind.has_value())) {
    Log::Error("Cannot open volume");
    return false;
  }

  if (options.read_ahead_bytes != 0) {
    volume->EnableReadAhead(options.read_ahead_bytes,
                            options.tuning.readahead.window_chunks);
  }
  return ParseBootSector();
}
#endif

// Use an already-open reader (eg. a test double), get BPB
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::OpenVolume(std::unique_ptr<IDiskReader> reader) {
  volume = std::make_unique<Io::FileReader<S>>(
      std::move(reader), options.max_cache_bytes, options.tuning);
  if (options.read_ahead_bytes != 0) {
    volume->EnableReadAhead(options.read_ahead_bytes,
                            options.tuning.readahead.window_chunks);
  }

  return ParseBootSector();
}

template <Cache::Strategy S>
Tuning::MediaKind NtfsVolume<S>::Impl::EffectiveMediaKind() const {
  return options.media_kind.value_or(volume->GetMediaKind());
}

// Read the first sector (boot sector) and derive volume geometry from it. When
// recovering and that sector is unusable, falls back on the backup boot sector.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::ParseBootSector() {
  if (Bench::Enabled()) {
    if (const std::optional<ULONGLONG> bytes = volume->Size()) {
      Bench::Detail::NoteVolumeBytes(*bytes);
    }
    Bench::Detail::NoteMediaKind(EffectiveMediaKind());
  }
  const Volume::BootSectorParse parse =
      Volume::BootSectorLoader<S>(*volume, options.recover_errors).Load();
  geometry = parse.geometry;
  // Sized from the decoded cluster, even when a later field is rejected.
  cluster_buffer.resize(geometry.cluster_size);
  return parse.ok;
}

// Verify NTFS volume version (must >= 3.0) and locate $MFT's Data attribute
template <Cache::Strategy S>
void NtfsVolume<S>::Impl::Init() {
  const Volume::VolumeRecordParse record =
      Volume::VolumeRecordLoader<S>(*self).Load();
  version_major = record.version_major;
  version_minor = record.version_minor;
  if (!record.ok || !mft.ParseRecord()) {
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

template <Cache::Strategy S>
Mft::MetadataOpener<S> NtfsVolume<S>::Impl::MetadataFiles() const {
  return Mft::MetadataOpener<S>(*self, volume_ok ? &mft.Map() : nullptr);
}

template <Cache::Strategy S>
Alloc::ClusterMap<S>& NtfsVolume<S>::Impl::Clusters() const {
  if (!clusters) {
    clusters = std::make_unique<Alloc::ClusterMap<S>>(
        Alloc::AllocLoader<S>(MetadataFiles(), *volume, geometry, options),
        options.recover_errors, geometry.cluster_size);
  }
  return *clusters;
}

// Loads $UpCase on first use. A failure is cached: the built-in mapping
// answers every later call.
template <Cache::Strategy S>
const UpCase::Table& NtfsVolume<S>::Impl::GetUpCaseTable() const {
  if (!upcase_loaded) {
    upcase_loaded = true;
    upcase = UpCase::TableLoader<S>(MetadataFiles()).Load();
    if (!upcase) {
      Log::Info("$UpCase is not usable: names collate by the built-in mapping");
    }
  }
  return upcase ? *upcase : UpCase::Table::BuiltIn();
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
ULONGLONG NtfsVolume<S>::GetSerialNumber() const noexcept {
  return impl_->geometry.serial;
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
void NtfsVolume<S>::SetReadPattern(Tuning::Pattern pattern) const {
  impl_->volume->SetReadPattern(pattern);
}

template <Cache::Strategy S>
Tuning::MediaKind NtfsVolume<S>::GetMediaKind() const {
  return impl_->EffectiveMediaKind();
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
std::vector<Mft::DiskRange>
    NtfsVolume<S>::Impl::MftSlotRanges(ULONGLONG first_ref,
                                       ULONGLONG count) const {
  if (first_ref < static_cast<ULONGLONG>(Mft::Idx::User) ||
      !self->HasMftBase() || count == 0) {
    return {};
  }

  const std::optional<ULONGLONG> offset =
      Mft::RecordOffset(self->GetFileRecordSize(), first_ref);
  const ULONGLONG record_size = self->GetFileRecordSize();
  if (!offset || record_size == 0 ||
      count > std::numeric_limits<ULONGLONG>::max() / record_size) {
    return {};
  }
  return mft.Map().DiskRangesOf(*offset, count * record_size);
}

template <Cache::Strategy S>
void NtfsVolume<S>::PrefetchMftSlots(ULONGLONG first_ref,
                                     ULONGLONG count) const {
  for (const Mft::DiskRange& range : impl_->MftSlotRanges(first_ref, count)) {
    impl_->volume->Prefetch(range.offset, range.length);
  }
}

template <Cache::Strategy S>
void NtfsVolume<S>::PrefetchMftSlotsStanding(ULONGLONG first_ref,
                                             ULONGLONG count) const {
  for (const Mft::DiskRange& range : impl_->MftSlotRanges(first_ref, count)) {
    impl_->volume->PrefetchStanding(range.offset, range.length);
  }
}

template <Cache::Strategy S>
void NtfsVolume<S>::CancelMftSlotsStanding(ULONGLONG first_ref,
                                           ULONGLONG count) const {
  for (const Mft::DiskRange& range : impl_->MftSlotRanges(first_ref, count)) {
    impl_->volume->CancelStanding(range.offset, range.length);
  }
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
  impl_->Clusters().States(queries);
}

template <Cache::Strategy S>
void NtfsVolume<S>::GetClusterRuns(ULONGLONG lcn, ULONGLONG clusters,
                                   std::vector<Alloc::ClusterRun>& runs) const {
  impl_->Clusters().Runs(lcn, clusters, runs);
}

template <Cache::Strategy S>
void NtfsVolume<S>::KeepBitmapWindows(ULONGLONG max_bytes) const {
  impl_->Clusters().KeepWindows(max_bytes);
}

template <Cache::Strategy S>
std::optional<Alloc::SpaceSummary> NtfsVolume<S>::GetSpaceSummary() const {
  return impl_->Clusters().Summary();
}

template <Cache::Strategy S>
const Alloc::BadClusters* NtfsVolume<S>::GetBadClusters() const {
  return impl_->Clusters().Bad();
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
