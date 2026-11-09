#pragma once

#include <ntfs-browser/namespace.h>

#include "attr/type.h"
#include "win-types.h"

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
