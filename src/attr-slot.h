#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/mask.h>

// Attribute Type to Index, eg. 0x10->0, 0x30->2
#define ATTR_INDEX(at) ((static_cast<DWORD>(at) >> 4U) - 1)
#define ATTR_MASK_(at) ((1U) << ATTR_INDEX(at))  // Attribute Bit Mask

// Attribute Bit Mask
#define ATTR_MASK(at) static_cast<Mask>(1U << ATTR_INDEX(at))

namespace NtfsBrowser
{

// Number of attribute types, so the size of any per-type table. It is one
// slot per multiple of 0x10 from 0x10 (STANDARD_INFORMATION) to 0x100
// (LOGGED_UTILITY_STREAM).
constexpr size_t kAttrNums = 16;

static_assert(static_cast<DWORD>(Mask::STANDARD_INFORMATION) ==
              ATTR_MASK_(AttrType::STANDARD_INFORMATION));
static_assert(static_cast<DWORD>(Mask::DATA) == ATTR_MASK_(AttrType::DATA));
static_assert(static_cast<DWORD>(Mask::LOGGED_UTILITY_STREAM) ==
              ATTR_MASK_(AttrType::LOGGED_UTILITY_STREAM));

// True only if "at" is a real AttrType value, not on-disk data that could
// alias another type's ATTR_INDEX/ATTR_MASK slot. Callers MUST check this
// before passing a value read from disk to ATTR_INDEX or ATTR_MASK.
[[nodiscard]] constexpr bool IsValidAttrType(AttrType at) noexcept
{
  const DWORD raw = static_cast<DWORD>(at);
  return raw != 0 && (raw & 0xFU) == 0 &&
         raw <= static_cast<DWORD>(AttrType::LOGGED_UTILITY_STREAM);
}

}  // namespace NtfsBrowser
