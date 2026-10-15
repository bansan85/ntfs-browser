#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Attr {

struct HeaderCommon {
  Type type;          // Attribute Type
  DWORD total_size;   // Length (including this header)
  BYTE non_resident;  // 0 - resident, 1 - non resident
  BYTE name_length;   // name length in words
  WORD name_offset;   // offset to the name
  WORD flags;         // Flags
  WORD id;            // Attribute Id
};

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
