#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{

enum class AttrType : DWORD
{
  // Zero-initialised value, never found on disk
  None = 0,

  // Attribute Header

  StandardInformation = 0x10,
  AttributeList = 0x20,
  FileName = 0x30,
  ObjectId = 0x40,
  SecurityDescriptor = 0x50,
  VolumeName = 0x60,
  VolumeInformation = 0x70,
  Data = 0x80,
  IndexRoot = 0x90,
  IndexAllocation = 0xA0,
  Bitmap = 0xB0,
  ReparsePoint = 0xC0,
  EaInformation = 0xD0,
  Ea = 0xE0,
  PropertySet = 0xF0,
  LoggedUtilityStream = 0x100,
  All = static_cast<DWORD>(-1)
};

}  // namespace NtfsBrowser