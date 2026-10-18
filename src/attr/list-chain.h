#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Attr {

// Width of the Attr::Type field in a chain key. Every Attr::Type fits 16 bits.
inline constexpr unsigned chain_key_type_bits = 16;

// Selects the Attr::Type bits of a chain key.
inline constexpr ULONGLONG chain_key_type_mask = 0xFFFFU;

// Packs (record_ref, attr_type) into one key. record_ref fits the high 48
// bits (MftSegmentReference::segment_number is 48-bit); every Attr::Type
// fits the low 16 bits.
constexpr ULONGLONG MakeChainKey(ULONGLONG record_ref,
                                 Type attr_type) noexcept {
  return (record_ref << chain_key_type_bits) |
         (static_cast<ULONGLONG>(attr_type) & chain_key_type_mask);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
