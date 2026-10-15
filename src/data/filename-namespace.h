#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Data {

enum class FilenameNamespace : BYTE { Posix = 0x00, Win32 = 0x01, Dos = 0x02 };

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NTFS_BROWSER_NAMESPACE::Data::FilenameNamespace)

}  // namespace NTFS_BROWSER_NAMESPACE::Data
