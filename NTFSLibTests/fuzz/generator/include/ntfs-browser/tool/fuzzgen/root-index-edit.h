#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Puts a hand-made index into the root directory record (5) of a fake image:
// a resident $INDEX_ROOT of file names, and the $INDEX_ALLOCATION behind it.
class RootIndexEdit {
 public:
  // What a hand-made $INDEX_ROOT holds.
  struct Spec {
    // The size of an index block, in bytes and in clusters.
    DWORD block_size = 0x400;
    BYTE clusters_per_block = 1;
    // An end entry that points to the sub-node at this VCN, or none.
    std::optional<ULONGLONG> sub_node_vcn;
    // A leaf entry for "AB", before the end entry.
    bool with_ab_entry = false;
    // The attributes the root already has stay, and the $INDEX_ROOT follows
    // them. Otherwise they go.
    bool keep_attributes = false;
  };

  // Puts the $INDEX_ROOT described by spec into the root record. Returns the
  // offset of its first entry.
  static std::size_t Put(std::vector<BYTE>& image, const Spec& spec);

  // Puts a non-resident $INDEX_ALLOCATION after the attributes of the root.
  // The bytes of runs are the run list, with its end. The attribute spans VCN 0
  // to last_vcn, and size is its allocated and real size.
  static void PutAllocation(std::vector<BYTE>& image,
                            const std::vector<BYTE>& runs, ULONGLONG last_vcn,
                            ULONGLONG size);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
