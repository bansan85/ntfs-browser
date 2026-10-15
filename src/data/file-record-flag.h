#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Data {

enum class FileRecordFlag : std::uint8_t {
  None = 0x00,
  InUse = 0x01,
  Dir = 0x02
};

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag)

}  // namespace NTFS_BROWSER_NAMESPACE::Data
