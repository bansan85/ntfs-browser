#pragma once

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser::Data {

// The "INDX" signature that opens an index block, read as a little-endian
// DWORD.
inline constexpr DWORD index_block_magic = 'XDNI';

struct IndexBlock {
  // Index Block Header
  DWORD magic;        // "INDX"
  WORD offset_of_us;  // Offset of Update Sequence
  WORD size_of_us;    // Size in words of Update Sequence Number & Array
  ULONGLONG lsn;      // $LogFile Sequence Number
  ULONGLONG vcn;      // VCN of this index block in the index allocation
  // Index Header
  DWORD entry_offset;  // Offset of the index entries,
  // relative to this address(0x18)
  DWORD total_entry_size;  // Total size of the index entries
  DWORD alloc_entry_size;  // Allocated size of index entries
  BYTE not_leaf;           // 1 if not leaf node (has children)
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  BYTE padding[3];  // Padding
};

}  // namespace NtfsBrowser::Data
