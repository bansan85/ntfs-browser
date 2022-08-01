#pragma once

#include <windows.h>

namespace NtfsBrowser
{

struct AttrVolumeInformation
{
  BYTE Reserved1[8];  // Always 0 ?
  BYTE MajorVersion;  // Major version
  BYTE MinorVersion;  // Minor version
  WORD Flags;         // Flags
  BYTE Reserved2[4];  // Always 0 ?
};

}  // namespace NtfsBrowser