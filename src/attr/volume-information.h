#pragma once

#include <windows.h>

// OK

namespace NtfsBrowser::Attr
{

struct VolumeInformation
{
  ULONGLONG Reserved1;  // Always 0 ?
  BYTE MajorVersion;    // Major version
  BYTE MinorVersion;    // Minor version
  WORD Flags;           // Flags
  DWORD Reserved2;      // Always 0 ?
};

}  // namespace NtfsBrowser::Attr