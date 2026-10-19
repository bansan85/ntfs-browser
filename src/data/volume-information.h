#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Data {

// Without this, alignof(ULONGLONG) pads sizeof() to 16, not the real
// 12-byte on-disk size.
#pragma pack(1)

struct VolumeInformation {
  ULONGLONG reserved1;  // Always 0 ?
  BYTE major_version;   // Major version
  BYTE minor_version;   // Minor version
  WORD flags;           // Flags
};

#pragma pack()

// Bit 0 of VolumeInformation::flags: the volume was not unmounted cleanly and
// its $LogFile was not replayed.
inline constexpr WORD volume_dirty_flag = 0x0001;

}  // namespace NTFS_BROWSER_NAMESPACE::Data
