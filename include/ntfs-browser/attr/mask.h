#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Attr {

enum class Mask : DWORD {
  // Bit masks of Attributes: the bit for attribute type T is
  // 1 << ((T >> 4) - 1), so each type has its own bit.
  StandardInformation = 0x0001,
  AttributeList = 0x0002,
  FileName = 0x0004,
  ObjectId = 0x0008,
  SecurityDescriptor = 0x0010,
  VolumeName = 0x0020,
  VolumeInformation = 0x0040,
  Data = 0x0080,
  IndexRoot = 0x0100,
  IndexAllocation = 0x0200,
  Bitmap = 0x0400,
  ReparsePoint = 0x0800,
  EaInformation = 0x1000,
  Ea = 0x2000,
  PropertySet = 0x4000,
  LoggedUtilityStream = 0x8000,
  All = static_cast<DWORD>(-1)
};

// NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(Mask)

}  // namespace NtfsBrowser::Attr
