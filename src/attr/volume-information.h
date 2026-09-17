#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Attr
{

// Without this, alignof(ULONGLONG) pads sizeof() to 16, not the real
// 12-byte on-disk size.
#pragma pack(1)
struct VolumeInformation
{
  ULONGLONG reserved1;  // Always 0 ?
  BYTE major_version;   // Major version
  BYTE minor_version;   // Minor version
  WORD flags;           // Flags
};
#pragma pack()

}  // namespace NtfsBrowser::Attr