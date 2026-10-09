#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Data {

enum class FilenameFlag : DWORD {
  None = 0x00000000,
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
  Encrypted = 0x00004000,
  Directory = 0x10000000,
  IndexView = 0x20000000
};

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Data::FilenameFlag)

}  // namespace NtfsBrowser::Data
