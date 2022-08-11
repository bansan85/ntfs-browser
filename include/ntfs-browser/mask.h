#pragma once

#include <windows.h>

#include <ntfs-browser/data/attr-type.h>

#define ATTR_INDEX(at) \
  (((at) >> 4U) - 1)  // Attribute Type to Index, eg. 0x10->0, 0x30->2
#define ATTR_MASK_(at) (((DWORD)1) << ATTR_INDEX(at))  // Attribute Bit Mask

namespace NtfsBrowser
{

enum class Mask : DWORD
{
  // Bit masks of Attributes
  STANDARD_INFORMATION =
      ATTR_MASK_(static_cast<DWORD>(AttrType::STANDARD_INFORMATION)),
  ATTRIBUTE_LIST = ATTR_MASK_(static_cast<DWORD>(AttrType::ATTRIBUTE_LIST)),
  FILE_NAME = ATTR_MASK_(static_cast<DWORD>(AttrType::FILE_NAME)),
  OBJECT_ID = ATTR_MASK_(static_cast<DWORD>(AttrType::OBJECT_ID)),
  SECURITY_DESCRIPTOR =
      ATTR_MASK_(static_cast<DWORD>(AttrType::SECURITY_DESCRIPTOR)),
  VOLUME_NAME = ATTR_MASK_(static_cast<DWORD>(AttrType::VOLUME_NAME)),
  VOLUME_INFORMATION =
      ATTR_MASK_(static_cast<DWORD>(AttrType::VOLUME_INFORMATION)),
  DATA = ATTR_MASK_(static_cast<DWORD>(AttrType::DATA)),
  INDEX_ROOT = ATTR_MASK_(static_cast<DWORD>(AttrType::INDEX_ROOT)),
  INDEX_ALLOCATION = ATTR_MASK_(static_cast<DWORD>(AttrType::INDEX_ALLOCATION)),
  BITMAP = ATTR_MASK_(static_cast<DWORD>(AttrType::BITMAP)),
  REPARSE_POINT = ATTR_MASK_(static_cast<DWORD>(AttrType::REPARSE_POINT)),
  EA_INFORMATION = ATTR_MASK_(static_cast<DWORD>(AttrType::EA_INFORMATION)),
  EA = ATTR_MASK_(static_cast<DWORD>(AttrType::EA)),
  LOGGED_UTILITY_STREAM =
      ATTR_MASK_(static_cast<DWORD>(AttrType::LOGGED_UTILITY_STREAM))
};

//NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Mask)

#define ATTR_MASK(at) \
  static_cast<Mask>(((DWORD)1) << ATTR_INDEX(at))  // Attribute Bit Mask

#define MASK_ALL static_cast<Mask>(static_cast<DWORD>(-1))

}  // namespace NtfsBrowser
