#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Data {

// NOLINTNEXTLINE(performance-enum-size)
enum class StdInfoPermission : DWORD {
  ReadOnly = 0x00000001,
  Hidden = 0x00000002,
  System = 0x00000004,
  Archive = 0x00000020,
  Device = 0x00000040,
  Normal = 0x00000080,
  Temp = 0x00000100,
  Sparse = 0x00000200,
  Reparse = 0x00000400,
  Compressed = 0x00000800,
  Offline = 0x00001000,
  Nci = 0x00002000,
  Encrypted = 0x00004000
};

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission)

}  // namespace NTFS_BROWSER_NAMESPACE::Data
