#pragma once

#include <ntfs-browser/namespace.h>

#include "attr/header-common.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Data {

struct HeaderResident {
  Attr::HeaderCommon header;  // Common data structure
  DWORD attr_size;            // Length of the attribute body
  WORD attr_offset;           // Offset to the Attribute
  BYTE indexed_flag;          // Indexed flag
  BYTE padding;               // Padding
};

}  // namespace NTFS_BROWSER_NAMESPACE::Data
