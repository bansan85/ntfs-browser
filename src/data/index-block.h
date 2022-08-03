#pragma once

#include <windows.h>

#define INDEX_BLOCK_MAGIC 'XDNI'

namespace NtfsBrowser::Data
{

struct IndexBlock
{
  // Index Block Header
  DWORD Magic;      // "INDX"
  WORD OffsetOfUS;  // Offset of Update Sequence
  WORD SizeOfUS;    // Size in words of Update Sequence Number & Array
  ULONGLONG LSN;    // $LogFile Sequence Number
  ULONGLONG VCN;    // VCN of this index block in the index allocation
  // Index Header
  DWORD EntryOffset;  // Offset of the index entries,
  // relative to this address(0x18)
  DWORD TotalEntrySize;  // Total size of the index entries
  DWORD AllocEntrySize;  // Allocated size of index entries
  BYTE NotLeaf;          // 1 if not leaf node (has children)
  BYTE Padding[3];       // Padding
};

}  // namespace NtfsBrowser::Data