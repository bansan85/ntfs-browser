#include <windows.h>

#include <ntfs-browser/namespace.h>

#include "util/local-file-time.h"

namespace NTFS_BROWSER_NAMESPACE::Util {

void UtcToLocalFileTime(ULONGLONG utc, FILETIME& local) noexcept {
  const _ULARGE_INTEGER fti{.QuadPart = utc};
  FILETIME ftt{.dwLowDateTime = fti.LowPart, .dwHighDateTime = fti.HighPart};

  if (FileTimeToLocalFileTime(&ftt, &local) == 0) {
    local = ftt;
  }
}

ULONGLONG LocalToUtcTicks(const FILETIME& local) noexcept {
  const auto ticks_of = [](const FILETIME& time) {
    return _ULARGE_INTEGER{.LowPart = time.dwLowDateTime,
                           .HighPart = time.dwHighDateTime}
        .QuadPart;
  };
  if (ticks_of(local) == 0) {
    return 0;
  }
  FILETIME utc{};
  if (LocalFileTimeToFileTime(&local, &utc) == 0) {
    return ticks_of(local);
  }
  return ticks_of(utc);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Util
