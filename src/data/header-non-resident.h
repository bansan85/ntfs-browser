#pragma once

#include <ntfs-browser/win-types.h>

#include <cstring>
#include <limits>
#include <span>

#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Data {

struct HeaderNonResident {
  Attr::HeaderCommon header;  // Common data structure
  ULONGLONG start_vcn;        // Starting VCN
  ULONGLONG last_vcn;         // Last VCN
  WORD data_run_offset;       // Offset to the Data Runs
  WORD comp_unit_size;        // Compression unit size
  DWORD padding;              // Padding
  ULONGLONG alloc_size;       // Allocated size of the attribute
  ULONGLONG real_size;        // Real size of the attribute
  ULONGLONG ini_size;         // Initialized data size of the stream
  // CompressedSize (8 bytes) follows here only when comp_unit_size != 0; not
  // a member, to keep the base header at 64 bytes - read via CompressedSize()
  // below.
};

// Size (bytes) of the trailing CompressedSize field described above.
inline constexpr DWORD compressed_size_field_size =
    static_cast<DWORD>(sizeof(ULONGLONG));

// Base header size (64 bytes, no CompressedSize); named so ParseAttrs()'s
// two size gates read clearly.
inline constexpr DWORD header_non_resident_base_size =
    static_cast<DWORD>(sizeof(HeaderNonResident));

// True if this attribute is compressed and therefore declares the trailing
// CompressedSize field.
[[nodiscard]] inline bool
    HasCompressedSizeField(const HeaderNonResident& header) noexcept {
  return header.comp_unit_size != 0;
}

// Reads the trailing CompressedSize field. Valid only once
// HasCompressedSizeField(header) is true and total_size was checked to cover
// header_non_resident_base_size + compressed_size_field_size bytes.
[[nodiscard]] inline ULONGLONG
    CompressedSize(const HeaderNonResident& header) noexcept {
  ULONGLONG size = 0;
  const std::span<const BYTE> field(reinterpret_cast<const BYTE*>(&header),
                                    header_non_resident_base_size +
                                        compressed_size_field_size);
  std::memcpy(&size, field.subspan(header_non_resident_base_size).data(),
              sizeof(size));
  return size;
}

// last_vcn minus start_vcn, as an unsigned count. last_vcn is inclusive, so
// an empty attribute stores it as -1 (all ones) with start_vcn 0: that is
// the maximum here. An inverted range wraps mod 2^64, as it always has, and
// is computed here without unsigned overflow.
[[nodiscard]] constexpr ULONGLONG
    VcnSpan(const HeaderNonResident& header) noexcept {
  constexpr ULONGLONG max = (std::numeric_limits<ULONGLONG>::max)();
  if (header.last_vcn >= header.start_vcn) {
    return header.last_vcn - header.start_vcn;
  }
  return max - (header.start_vcn - header.last_vcn) + 1;
}

// Clusters an attribute header spans. The maximum span is the empty
// attribute, so it counts as 0 clusters, not a wrapped count.
[[nodiscard]] constexpr ULONGLONG
    SpannedClusters(const HeaderNonResident& header) noexcept {
  const ULONGLONG span = VcnSpan(header);
  return span == (std::numeric_limits<ULONGLONG>::max)() ? 0 : span + 1;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Data
