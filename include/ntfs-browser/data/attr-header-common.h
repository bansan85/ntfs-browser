#pragma once

#include <windows.h>

namespace NtfsBrowser
{
struct AttrHeaderCommon
{
  DWORD Type;        // Attribute Type
  DWORD TotalSize;   // Length (including this header)
  BYTE NonResident;  // 0 - resident, 1 - non resident
  BYTE NameLength;   // name length in words
  WORD NameOffset;   // offset to the name
  WORD Flags;        // Flags
  WORD Id;           // Attribute Id
};
}  // namespace NtfsBrowser
