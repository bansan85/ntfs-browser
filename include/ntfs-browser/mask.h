#pragma once

#include <windows.h>

#include <ntfs-browser/data/attr-type.h>

// Attribute Type to Index, eg. 0x10->0, 0x30->2
#define ATTR_INDEX(at) ((static_cast<DWORD>(at) >> 4U) - 1)
#define ATTR_MASK_(at) ((1U) << ATTR_INDEX(at))  // Attribute Bit Mask

namespace NtfsBrowser
{

enum class Mask : DWORD
{
  // Bit masks of Attributes
  STANDARD_INFORMATION = ATTR_MASK_(AttrType::STANDARD_INFORMATION),
  ATTRIBUTE_LIST = ATTR_MASK_(AttrType::ATTRIBUTE_LIST),
  FILE_NAME = ATTR_MASK_(AttrType::FILE_NAME),
  OBJECT_ID = ATTR_MASK_(AttrType::OBJECT_ID),
  SECURITY_DESCRIPTOR = ATTR_MASK_(AttrType::SECURITY_DESCRIPTOR),
  VOLUME_NAME = ATTR_MASK_(AttrType::VOLUME_NAME),
  VOLUME_INFORMATION = ATTR_MASK_(AttrType::VOLUME_INFORMATION),
  DATA = ATTR_MASK_(AttrType::DATA),
  INDEX_ROOT = ATTR_MASK_(AttrType::INDEX_ROOT),
  INDEX_ALLOCATION = ATTR_MASK_(AttrType::INDEX_ALLOCATION),
  BITMAP = ATTR_MASK_(AttrType::BITMAP),
  REPARSE_POINT = ATTR_MASK_(AttrType::REPARSE_POINT),
  EA_INFORMATION = ATTR_MASK_(AttrType::EA_INFORMATION),
  EA = ATTR_MASK_(AttrType::EA),
  PROPERTY_SET = ATTR_MASK_(AttrType::PROPERTY_SET),
  LOGGED_UTILITY_STREAM = ATTR_MASK_(AttrType::LOGGED_UTILITY_STREAM),
  ALL = static_cast<DWORD>(-1)
};

//NOLINTNEXTLINE
DEFINE_ENUM_FLAG_OPERATORS(NtfsBrowser::Mask)

// Attribute Bit Mask
#define ATTR_MASK(at) static_cast<Mask>(1U << ATTR_INDEX(at))

}  // namespace NtfsBrowser
