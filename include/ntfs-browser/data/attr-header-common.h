#pragma once

#include <windows.h>

#include <ntfs-browser/data/attr-type.h>

namespace NtfsBrowser
{
struct AttrHeaderCommon
{
  AttrType type;      // Attribute Type
  DWORD total_size;   // Length (including this header)
  BYTE non_resident;  // 0 - resident, 1 - non resident
  BYTE name_length;   // name length in words
  WORD name_offset;   // offset to the name
  WORD flags;         // Flags
  WORD id;            // Attribute Id
};
}  // namespace NtfsBrowser
