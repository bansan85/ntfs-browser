#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{

enum class Mask : DWORD
{
  // Bit masks of Attributes: the bit for attribute type T is
  // 1 << ((T >> 4) - 1), so each type has its own bit.
  STANDARD_INFORMATION = 0x0001,
  ATTRIBUTE_LIST = 0x0002,
  FILE_NAME = 0x0004,
  OBJECT_ID = 0x0008,
  SECURITY_DESCRIPTOR = 0x0010,
  VOLUME_NAME = 0x0020,
  VOLUME_INFORMATION = 0x0040,
  DATA = 0x0080,
  INDEX_ROOT = 0x0100,
  INDEX_ALLOCATION = 0x0200,
  BITMAP = 0x0400,
  REPARSE_POINT = 0x0800,
  EA_INFORMATION = 0x1000,
  EA = 0x2000,
  PROPERTY_SET = 0x4000,
  LOGGED_UTILITY_STREAM = 0x8000,
  ALL = static_cast<DWORD>(-1)
};

//NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Mask)

}  // namespace NtfsBrowser
