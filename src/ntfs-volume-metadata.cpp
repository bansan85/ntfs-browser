#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr/resident.h"
#include "attr/vol-info.h"
#include "attr/vol-name.h"
#include "data/volume-information.h"
#include "io/file-record-attr-types.h"
#include "log/ntfs-common.h"
#include "ntfs-volume-impl.h"
#include "upcase/upcase.h"
#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE {

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

}  // namespace

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

// Whether the volume is open and $MFT maps the record of a metadata file.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::IsMetadataMapped(ULONGLONG record) const {
  const DWORD record_size = geometry.file_record_size;
  return volume_ok &&
         mft.Map().IsRangeMapped(record * record_size, record_size);
}

// Parses record as MFT record number index, with its $DATA attributes only.
// Like the volume's other metadata reads, it MUST NOT depend on
// include_deleted.
template <Cache::Strategy S>
bool NtfsVolume<S>::Impl::ParseMetadataData(Io::FileRecord<S>& record,
                                            ULONGLONG index) const {
  record.SetBypassDeletedGate(true);
  record.SetAttrMask(Attr::Mask::Data);
  return record.ParseFileRecord(index) && record.ParseAttrs();
}

// Reads $UpCase (MFT record 10). Null when it is missing or unusable.
template <Cache::Strategy S>
std::unique_ptr<const UpCase::Table>
    NtfsVolume<S>::Impl::LoadUpCaseTable() const {
  const auto upcase_record = static_cast<ULONGLONG>(Mft::Idx::UpCase);
  if (!IsMetadataMapped(upcase_record)) {
    return {};
  }

  Io::FileRecord<S> record(*self);
  if (!ParseMetadataData(record, upcase_record)) {
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

template void NtfsVolume<Cache::Strategy::NoCache>::Impl::Init();
template void NtfsVolume<Cache::Strategy::FullCache>::Impl::Init();
template bool NtfsVolume<Cache::Strategy::NoCache>::Impl::IsMetadataMapped(
    ULONGLONG record) const;
template bool NtfsVolume<Cache::Strategy::FullCache>::Impl::IsMetadataMapped(
    ULONGLONG record) const;
template bool NtfsVolume<Cache::Strategy::NoCache>::Impl::ParseMetadataData(
    Io::FileRecord<Cache::Strategy::NoCache>& record, ULONGLONG index) const;
template bool NtfsVolume<Cache::Strategy::FullCache>::Impl::ParseMetadataData(
    Io::FileRecord<Cache::Strategy::FullCache>& record, ULONGLONG index) const;
template const UpCase::Table&
    NtfsVolume<Cache::Strategy::NoCache>::Impl::GetUpCaseTable() const;
template const UpCase::Table&
    NtfsVolume<Cache::Strategy::FullCache>::Impl::GetUpCaseTable() const;

}  // namespace NTFS_BROWSER_NAMESPACE
