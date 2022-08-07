#pragma once

#include <windows.h>

#include <ntfs-browser/data/attr-header-common.h>

namespace NtfsBrowser::Attr
{
struct HeaderResident
{
  AttrHeaderCommon Header;  // Common data structure
  DWORD AttrSize;           // Length of the attribute body
  WORD AttrOffset;          // Offset to the Attribute
  BYTE IndexedFlag;         // Indexed flag
  BYTE padding;             // Padding
};
}  // namespace NtfsBrowser::Attr
