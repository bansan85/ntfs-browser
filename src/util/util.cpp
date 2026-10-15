#include "util/util.h"

#include <ntfs-browser/win-types.h>

#include <span>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Util {

void SecureZero(std::span<BYTE> bytes) noexcept {
  // The volatile write keeps the compiler from eliding the wipe.
  for (BYTE& byte : bytes)  // NOLINT(misc-const-correctness)
  {
    *static_cast<volatile BYTE*>(&byte) = 0;
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Util
