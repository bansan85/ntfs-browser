#pragma once

#include <span>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Util {

// Overwrites key material in a way the compiler cannot drop as a dead store.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SecureZero(std::span<BYTE> bytes) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Util
