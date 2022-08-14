#pragma once

#include <windows.h>

namespace NtfsBrowser::Attr
{

struct VolumeInformation
{
  ULONGLONG reserved1;  // Always 0 ?
  BYTE major_version;   // Major version
  BYTE minor_version;   // Minor version
  WORD flags;           // Flags
  DWORD reserved2;      // Always 0 ?
};

}  // namespace NtfsBrowser::Attr