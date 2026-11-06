#include "volume/volume-record-loader.h"

#include <ntfs-browser/win-types.h>

#include <string_view>
#include <tuple>

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
#include "utf/utf.h"

namespace NTFS_BROWSER_NAMESPACE::Volume {

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

template <Cache::Strategy S>
VolumeRecordLoader<S>::VolumeRecordLoader(const NtfsVolume<S>& volume) noexcept
    : volume_(&volume) {}

template <Cache::Strategy S>
bool VolumeRecordLoader<S>::ParseRecord(Io::FileRecord<S>& vol) {
  vol.SetBypassDeletedGate(true);
  vol.SetAttrMask(Attr::Mask::VolumeName | Attr::Mask::VolumeInformation);
  if (!vol.ParseFileRecord(static_cast<DWORD>(Mft::Idx::Volume)) ||
      !vol.ParseAttrs()) {
    return false;
  }
  return !vol.GetAttr(Attr::Type::VolumeInformation).empty();
}

template <Cache::Strategy S>
VolumeRecordParse
    VolumeRecordLoader<S>::ReadVersion(const Io::FileRecord<S>& vol) {
  const auto* info =
      reinterpret_cast<const Attr::AttrVolInfo<Io::ResidentOf<S>, S>*>(
          vol.GetAttr(Attr::Type::VolumeInformation).front().get());
  VolumeRecordParse parse;
  std::tie(parse.version_major, parse.version_minor) = info->GetVersion();
  Log::Info("NTFS volume version: {}.{}", parse.version_major,
            parse.version_minor);
  if ((info->GetFlags() & Data::volume_dirty_flag) != 0) {
    Log::Warn(
        "NTFS volume was not unmounted cleanly: its journal was not "
        "replayed, so the data read may be stale");
  }
  parse.ok = parse.version_major >= 3;
  return parse;
}

template <Cache::Strategy S>
void VolumeRecordLoader<S>::LogName(const Io::FileRecord<S>& vol) {
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

template <Cache::Strategy S>
VolumeRecordParse VolumeRecordLoader<S>::Load() const {
  Io::FileRecord<S> vol(*volume_);
  if (!ParseRecord(vol)) {
    return {};
  }
  const VolumeRecordParse parse = ReadVersion(vol);
  if (parse.ok) {
    LogName(vol);
  }
  return parse;
}

template class VolumeRecordLoader<Cache::Strategy::NoCache>;
template class VolumeRecordLoader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Volume
