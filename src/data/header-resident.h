#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/attr/header-common.h>

namespace NtfsBrowser::Data {

struct HeaderResident {
  Attr::HeaderCommon header;  // Common data structure
  DWORD attr_size;            // Length of the attribute body
  WORD attr_offset;           // Offset to the Attribute
  BYTE indexed_flag;          // Indexed flag
  BYTE padding;               // Padding
};

}  // namespace NtfsBrowser::Data
