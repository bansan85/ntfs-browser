#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/mask.h>

namespace NtfsBrowser
{

// Number of attribute types, so the size of any per-type table. It is one
// slot per multiple of 0x10 from 0x10 (STANDARD_INFORMATION) to 0x100
// (LOGGED_UTILITY_STREAM).
constexpr size_t kAttrNums = 16;

// Attribute Type to Index, eg. 0x10->0, 0x30->2
[[nodiscard]] constexpr DWORD AttrIndex(AttrType at) noexcept
{
  return (static_cast<DWORD>(at) >> 4U) - 1;
}

// Attribute Bit Mask, as a plain integer.
[[nodiscard]] constexpr DWORD AttrMaskBits(AttrType at) noexcept
{
  return 1U << AttrIndex(at);
}

// Attribute Bit Mask
[[nodiscard]] constexpr Mask AttrMask(AttrType at) noexcept
{
  return static_cast<Mask>(AttrMaskBits(at));
}

static_assert(static_cast<DWORD>(Mask::STANDARD_INFORMATION) ==
              AttrMaskBits(AttrType::STANDARD_INFORMATION));
static_assert(static_cast<DWORD>(Mask::DATA) == AttrMaskBits(AttrType::DATA));
static_assert(static_cast<DWORD>(Mask::LOGGED_UTILITY_STREAM) ==
              AttrMaskBits(AttrType::LOGGED_UTILITY_STREAM));

// Every AttrType value is a multiple of 16: its low nibble is always zero.
inline constexpr DWORD kAttrTypeLowNibbleMask = 0xFU;

// True only if "at" is a real AttrType value, not on-disk data that could
// alias another type's AttrIndex/AttrMask slot. Callers MUST check this
// before passing a value read from disk to AttrIndex or AttrMask.
[[nodiscard]] constexpr bool IsValidAttrType(AttrType at) noexcept
{
  const auto raw = static_cast<DWORD>(at);
  return raw != 0 && (raw & kAttrTypeLowNibbleMask) == 0 &&
         raw <= static_cast<DWORD>(AttrType::LOGGED_UTILITY_STREAM);
}

}  // namespace NtfsBrowser
