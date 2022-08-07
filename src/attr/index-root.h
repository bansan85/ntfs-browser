#pragma once

#include <windows.h>

namespace NtfsBrowser::Attr
{

struct IndexRoot
{
  // Index Root Header
  DWORD attr_type;  // Attribute type
  //(ATTR_TYPE_FILE_NAME: Directory, 0: Index View)
  DWORD CollRule;      // Collation rule
  DWORD IBSize;        // Size of index block
  BYTE ClustersPerIB;  // Clusters per index block (same as BPB?)
  BYTE Padding1[3];    // Padding
  // Index Header
  DWORD EntryOffset;  // Offset to the first index entry,
  // relative to this address(0x10)
  DWORD TotalEntrySize;  // Total size of the index entries
  DWORD AllocEntrySize;  // Allocated size of the index entries
  BYTE Flags;            // Flags
  BYTE Padding2[3];      // Padding
};

}  // namespace NtfsBrowser::Attr