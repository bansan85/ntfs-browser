#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

// Writes the parts of a directory index as the first author did: entries of
// 8-byte alignment, each for a name in the root directory, and an index with
// the collation rule of file names.
class LegacyIndex {
 public:
  // The value of a $FILE_NAME for a file in the root directory. name holds
  // UTF-16 units, paired or not.
  [[nodiscard]] static std::vector<BYTE>
      FileNameValue(std::u16string_view name);

  // A leaf entry for the file name of the record number record.
  [[nodiscard]] static std::vector<BYTE> Entry(ULONGLONG record,
                                               std::u16string_view name);

  // The last entry of an index. It lists no file. With sub_node set it flags a
  // sub-node, and with vcn set it ends with the VCN of that sub-node.
  [[nodiscard]] static std::vector<BYTE>
      LastEntry(bool sub_node, std::optional<ULONGLONG> vcn = std::nullopt);

  // The value of an $INDEX_ROOT of file names with the entries given, the last
  // one included. children tells the index that it has sub-nodes.
  [[nodiscard]] static std::vector<BYTE>
      RootValue(const std::vector<BYTE>& entries, bool children);

  // An index block of one cluster for the VCN vcn, with the entries given.
  [[nodiscard]] static std::vector<BYTE>
      Block(ULONGLONG vcn, const std::vector<BYTE>& entries);
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
