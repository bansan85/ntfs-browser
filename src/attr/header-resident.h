#pragma once

#include <windows.h>

#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser::Attr
{
struct HeaderResident
{
  AttrHeaderCommon header;  // Common data structure
  DWORD attr_size;           // Length of the attribute body
  WORD attr_offset;          // Offset to the Attribute
  BYTE indexed_flag;         // Indexed flag
  BYTE padding;             // Padding
};
}  // namespace NtfsBrowser::Attr
