#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>

namespace NtfsBrowser::Flag {

enum class FileRecord : std::uint8_t { None = 0x00, InUse = 0x01, Dir = 0x02 };

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Flag::FileRecord)

}  // namespace NtfsBrowser::Flag
