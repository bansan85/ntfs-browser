#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Attr
{

// On-disk $VOLUME_INFORMATION is exactly 12 bytes (8 + 1 + 1 + 2); there is
// no trailing reserved DWORD. Without pack(1), alignof(ULONGLONG) pads
// sizeof() up to 16 - which made the size check in AttrVolInfo's ctor
// reject every real volume's (12-byte) attribute, even after the previously
// present trailing reserved2 field (added to match that inflated size) was
// removed.
#pragma pack(1)
struct VolumeInformation
{
  ULONGLONG reserved1;  // Always 0 ?
  BYTE major_version;   // Major version
  BYTE minor_version;   // Minor version
  WORD flags;           // Flags
  // DWORD reserved2;      // Always 0 ?
};
#pragma pack()

}  // namespace NtfsBrowser::Attr