#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/attr/type.h>

namespace NtfsBrowser::Data {

struct IndexRoot {
  // Index Root Header
  Attr::Type attr_type;  // Attribute type
  //(ATTR_TYPE_FILE_NAME: Directory, 0: Index View)
  DWORD coll_rule;       // Collation rule
  DWORD ib_size;         // Size of index block
  BYTE clusters_per_ib;  // Clusters per index block (same as BPB?)
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  BYTE padding1[3];  // Padding
  // Index Header
  DWORD entry_offset;  // Offset to the first index entry,
  // relative to this address(0x10)
  DWORD total_entry_size;  // Total size of the index entries
  DWORD alloc_entry_size;  // Allocated size of the index entries
  BYTE flags;              // Flags
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
  BYTE padding2[3];  // Padding
};

}  // namespace NtfsBrowser::Data
