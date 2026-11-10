#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "mft/entry-table.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

// How the entries of a table link into a tree: the records filed under each
// directory, and which entries the root reaches.
class EntryLinks {
 public:
  // Validates every name's parent reference, files each record under the
  // parents its valid names give, then marks what the root reaches. Returns
  // the number of entries the root does not reach.
  ULONGLONG Link(EntryTable& table);

  // The records filed under the directory dir_record, in the order of the
  // entries. A root whose record is lost still has its children.
  [[nodiscard]] std::span<const ULONGLONG> Children(const EntryTable& table,
                                                    ULONGLONG dir_record) const;

  // Whether the root reaches table.Entries()[index]. False before Link().
  [[nodiscard]] bool IsReachable(size_t index) const noexcept;

 private:
  // Files every entry under the parents its valid names give.
  void FileChildren(const EntryTable& table);

  // Marks what the root reaches through the lists FileChildren() made.
  void MarkReachable(const EntryTable& table);

  // The records filed under each entry, through a valid parent reference:
  // those of entries[i] are children_records_[children_first_[i]] up to
  // children_first_[i + 1]. A root whose record is lost has its own list.
  std::vector<size_t> children_first_;
  std::vector<ULONGLONG> children_records_;
  std::vector<ULONGLONG> lost_root_children_;
  // Parallel to the entries.
  std::vector<bool> reachable_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
