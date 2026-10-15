#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Data {

enum class IndexEntryFlag : BYTE {
  None = 0x00,
  SubNode = 0x01,  // Index entry points to a sub-node
  Last = 0x02      // Last index entry in the node, no Stream
};

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NTFS_BROWSER_NAMESPACE::Data::IndexEntryFlag)

}  // namespace NTFS_BROWSER_NAMESPACE::Data
