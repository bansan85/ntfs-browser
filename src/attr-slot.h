#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/mask.h>

namespace NtfsBrowser::Attr {

// Number of attribute types, so the size of any per-type table. It is one
// slot per multiple of 0x10 from 0x10 (STANDARD_INFORMATION) to 0x100
// (LOGGED_UTILITY_STREAM).
constexpr size_t attr_nums = 16;

// Attribute Type to Index, eg. 0x10->0, 0x30->2
[[nodiscard]] constexpr DWORD AttrIndex(Type type) noexcept {
  return (static_cast<DWORD>(type) >> 4U) - 1;
}

// Attribute Bit Mask, as a plain integer.
[[nodiscard]] constexpr DWORD AttrMaskBits(Type type) noexcept {
  return 1U << AttrIndex(type);
}

// Attribute Bit Mask
[[nodiscard]] constexpr Mask AttrMask(Type type) noexcept {
  return static_cast<Mask>(AttrMaskBits(type));
}

static_assert(static_cast<DWORD>(Mask::StandardInformation) ==
              AttrMaskBits(Type::StandardInformation));
static_assert(static_cast<DWORD>(Mask::Data) == AttrMaskBits(Type::Data));
static_assert(static_cast<DWORD>(Mask::LoggedUtilityStream) ==
              AttrMaskBits(Type::LoggedUtilityStream));

// Every Attr::Type value is a multiple of 16: its low nibble is always zero.
inline constexpr DWORD attr_type_low_nibble_mask = 0xFU;

// True only if "at" is a real Attr::Type value, not on-disk data that could
// alias another type's AttrIndex/AttrMask slot. Callers MUST check this
// before passing a value read from disk to AttrIndex or AttrMask.
[[nodiscard]] constexpr bool IsValidAttrType(Type attr_type) noexcept {
  const auto raw = static_cast<DWORD>(attr_type);
  return raw != 0 && (raw & attr_type_low_nibble_mask) == 0 &&
         raw <= static_cast<DWORD>(Type::LoggedUtilityStream);
}

}  // namespace NtfsBrowser::Attr
