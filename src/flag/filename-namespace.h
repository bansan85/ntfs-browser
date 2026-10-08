#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Flag {

enum class FilenameNamespace : BYTE { Posix = 0x00, Win32 = 0x01, Dos = 0x02 };

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Flag::FilenameNamespace)

}  // namespace NtfsBrowser::Flag
