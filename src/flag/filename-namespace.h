#pragma once

#include <windows.h>

// OK

namespace NtfsBrowser::Flag
{

enum class FilenameNamespace : BYTE
{
  POSIX = 0x00,
  WIN_32 = 0x01,
  DOS = 0x02
};

//NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Flag::FilenameNamespace)

}  // namespace NtfsBrowser::Flag