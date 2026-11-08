#pragma once

#include <ntfs-browser/win-types.h>

#include <vector>

#include <ntfs-browser/namespace.h>

#include "layer-line.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// The directory indexes of a volume that Windows formatted, built from named
// fields. An index holds entry lines: "entry" with ref (a file reference),
// flags (1 for a sub-node, 2 for the last entry), vcn (the sub-node, when the
// flags say so) and, for an entry that is not the last one, the fields of a
// $FILE_NAME (see AttributeValues::FileName).
class IndexLayer {
 public:
  // An index block of the line "indx", followed by its entries. The line gives
  // size (bytes), vcn, lsn, usn, not_leaf, entry_offset and alloc, all
  // numbers. entry_offset counts from the index header, as the block records
  // it. Only size is required. The bytes after the entries are zero.
  [[nodiscard]] static std::vector<BYTE>
      Block(const LayerLine& line, const std::vector<LayerLine>& entries);

  // The value of an $INDEX_ROOT of file names, from the line "indexroot" and
  // its entries. The line gives rule (the collation rule), block (the size of
  // an index block), clusters (clusters per block), children (1 when the index
  // has sub-nodes) and alloc (the allocated size of the entries), all
  // numbers. alloc defaults to the size of the entries.
  [[nodiscard]] static std::vector<BYTE>
      Root(const LayerLine& line, const std::vector<LayerLine>& entries);

 private:
  // The bytes of one entry line.
  [[nodiscard]] static std::vector<BYTE> Entry(const LayerLine& line);

  // The bytes of all the entries, one after the other.
  [[nodiscard]] static std::vector<BYTE>
      Entries(const std::vector<LayerLine>& lines);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
