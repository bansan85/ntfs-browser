#include <ntfs-browser/namespace.h>

#include "util/local-file-time.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Util {

namespace {

// Selects the low DWORD of a 64-bit FILETIME value.
constexpr ULONGLONG low_dword_mask = 0xFFFFFFFFULL;

// Bits in a DWORD: where the high half of a FILETIME value starts.
constexpr unsigned dword_bits = 32;

}  // namespace

void UtcToLocalFileTime(ULONGLONG utc, FILETIME& local) noexcept {
  local.dwLowDateTime = static_cast<DWORD>(utc & low_dword_mask);
  local.dwHighDateTime = static_cast<DWORD>(utc >> dword_bits);
}

ULONGLONG LocalToUtcTicks(const FILETIME& local) noexcept {
  return (static_cast<ULONGLONG>(local.dwHighDateTime) << dword_bits) |
         local.dwLowDateTime;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Util
