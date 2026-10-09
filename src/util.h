#pragma once

#include <ntfs-browser/win-types.h>

#include <span>

#include "internal-export.h"

namespace NtfsBrowser::Util {

// Overwrites key material in a way the compiler cannot drop as a dead store.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SecureZero(std::span<BYTE> bytes) noexcept;

}  // namespace NtfsBrowser::Util
