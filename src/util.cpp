#include "util.h"

#include <ntfs-browser/win-types.h>

#include <span>

namespace NtfsBrowser::Util {

void SecureZero(std::span<BYTE> bytes) noexcept {
  // The volatile write keeps the compiler from eliding the wipe.
  for (BYTE& byte : bytes)  // NOLINT(misc-const-correctness)
  {
    *static_cast<volatile BYTE*>(&byte) = 0;
  }
}

}  // namespace NtfsBrowser::Util
