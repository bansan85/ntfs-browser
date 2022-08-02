#pragma once

#include <windows.h>

namespace NtfsBrowser::Attr
{

struct VolumeInformation
{
  BYTE Reserved1[8];  // Always 0 ?
  BYTE MajorVersion;  // Major version
  BYTE MinorVersion;  // Minor version
  WORD Flags;         // Flags
  BYTE Reserved2[4];  // Always 0 ?
};

}  // namespace NtfsBrowser::Attr