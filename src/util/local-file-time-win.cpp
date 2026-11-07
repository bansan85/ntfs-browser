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

}  // namespace NTFS_BROWSER_NAMESPACE::Util
