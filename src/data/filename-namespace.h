#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Data {

enum class FilenameNamespace : BYTE { Posix = 0x00, Win32 = 0x01, Dos = 0x02 };

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Data::FilenameNamespace)

}  // namespace NtfsBrowser::Data
