#pragma once

#include <winnt.h>

namespace NtfsBrowser::Flag
{

enum class StdInfoPermission : DWORD
{
  READONLY = 0x00000001,
  HIDDEN = 0x00000002,
  SYSTEM = 0x00000004,
  ARCHIVE = 0x00000020,
  DEVICE = 0x00000040,
  NORMAL = 0x00000080,
  TEMP = 0x00000100,
  SPARSE = 0x00000200,
  REPARSE = 0x00000400,
  COMPRESSED = 0x00000800,
  OFFLINE = 0x00001000,
  NCI = 0x00002000,
  ENCRYPTED = 0x00004000
};

//NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Flag::StdInfoPermission)

}  // namespace NtfsBrowser::Flag