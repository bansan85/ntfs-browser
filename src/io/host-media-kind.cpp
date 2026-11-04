#include "io/host-media-kind.h"

#include <string>
#include <vector>

#include <windows.h>
#include <winioctl.h>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Asks the volume that hosts path. The handle has access 0, so the volume is
// never read, and it is closed at once. A network share, a path that is not on
// a local volume and a volume that does not answer are all Unknown.
Tuning::MediaKind AskHostVolume(const std::wstring& path) {
  // The root of a volume is never longer than a path under it.
  std::vector<wchar_t> root(path.size() + 2, L'\0');
  if (GetVolumePathNameW(path.c_str(), root.data(),
                         static_cast<DWORD>(root.size())) == FALSE) {
    return Tuning::MediaKind::Unknown;
  }

  // A GUID path always fits: \\?\Volume{...}\ is 49 units with its end.
  constexpr DWORD guid_path_units = 64;
  std::vector<wchar_t> name(guid_path_units, L'\0');
  if (GetVolumeNameForVolumeMountPointW(root.data(), name.data(),
                                        guid_path_units) == FALSE) {
    return Tuning::MediaKind::Unknown;
  }

  // The trailing backslash would name the root folder, not the volume device.
  std::wstring volume(name.data());
  if (volume.empty() || volume.back() != L'\\') {
    return Tuning::MediaKind::Unknown;
  }
  volume.pop_back();

  const HANDLE handle =
      CreateFileW(volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Tuning::MediaKind::Unknown;
  }
  const Tuning::MediaKind kind = SeekPenaltyKindOf(handle);
  CloseHandle(handle);
  return kind;
}

// The installed query. One thread uses it, as with the rest of the library.
HostMediaQuery& InstalledQuery() noexcept {
  static HostMediaQuery query = &AskHostVolume;
  return query;
}

}  // namespace

// The kind of device a handle names, from its seek penalty. Unknown when the
// handle refuses the query.
Tuning::MediaKind SeekPenaltyKindOf(void* handle) noexcept {
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceSeekPenaltyProperty;
  query.QueryType = PropertyStandardQuery;
  DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
  DWORD returned = 0;
  if (DeviceIoControl(static_cast<HANDLE>(handle), IOCTL_STORAGE_QUERY_PROPERTY,
                      &query, sizeof(query), &penalty, sizeof(penalty),
                      &returned, nullptr) == FALSE ||
      returned < sizeof(penalty)) {
    return Tuning::MediaKind::Unknown;
  }
  return penalty.IncursSeekPenalty != FALSE ? Tuning::MediaKind::Rotational
                                            : Tuning::MediaKind::SolidState;
}

Tuning::MediaKind QueryHostMediaKind(const std::wstring& path) {
  return InstalledQuery()(path);
}

HostMediaQuery SetHostMediaQueryForTests(HostMediaQuery query) noexcept {
  HostMediaQuery& installed = InstalledQuery();
  const HostMediaQuery previous = installed;
  installed = query != nullptr ? query : &AskHostVolume;
  return previous;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
