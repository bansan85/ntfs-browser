#pragma once

#include <windows.h>

namespace NtfsBrowser::Flag
{

enum class FilenameNamespace : BYTE
{
  POSIX = 0x00,
  WIN_32 = 0x01,
  DOS = 0x02
};

DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Flag::FilenameNamespace)

}  // namespace NtfsBrowser::Flag