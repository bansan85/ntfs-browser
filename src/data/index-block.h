#pragma once

#include <windows.h>

static constexpr DWORD kIndexBlockMagic = 'XDNI';

// OK

namespace NtfsBrowser::Data
{

struct IndexBlock
{
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
  BYTE padding[3];         // Padding
};

}  // namespace NtfsBrowser::Data