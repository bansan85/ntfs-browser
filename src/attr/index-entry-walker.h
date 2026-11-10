#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "data/index-entry.h"
#include "index-entry.h"
#include "internal-export.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Attr {

// Walks the index entries that follow a node header, in an index root or in an
// index block. Each one is bounded against the bytes that are left, checked,
// and appended as a view to the caller's entry list.
class NTFS_BROWSER_EXPORT_TESTS_ONLY IndexEntryWalker {
 public:
  // The log lines of one kind of node.
  struct Messages {
    // The bytes left cannot hold an entry header.
    std::string_view header_exceeds;
    // An entry is empty or longer than the bytes left.
    std::string_view entry_exceeds;
    // The entries add up to more than the node declares.
    std::string_view total_exceeds;
  };

  // The lines for the entries of an index block.
  static constexpr Messages block_messages{
      "Index Block: index entry header exceeds block bounds",
      "Index Block: index entry exceeds block bounds",
      "Index Block: index entry total exceeds the block's declared entry size"};

  // The lines for the entries of an index root.
  static constexpr Messages root_messages{
      "Index Root: index entry header exceeds attribute bounds",
      "Index Root: index entry exceeds attribute bounds",
      "Index Root: index entry total exceeds the attribute's declared entry "
      "size"};

  // How a walk ended.
  struct Outcome {
    // False when the node MUST be rejected. Strict mode has then emptied the
    // entry list, so no entry parsed so far stays.
    bool accepted;
    // Set only when the walk reached the end entry and no defect passed under
    // recovery: the bytes from the start of the walk to the end of that entry.
    // Else where the used part of the node ends is not known.
    std::optional<size_t> clean_end;
  };

  // entries receives a view of each entry. They view `realigned` for the
  // entries that sit at a misaligned address: it MUST outlive them. In strict
  // mode (recover false), a defect empties entries.
  IndexEntryWalker(const Messages& messages, bool recover,
                   std::vector<IndexEntryView>& entries,
                   std::vector<std::vector<BYTE>>& realigned) noexcept;

  // Walks the entries in first, which starts at the first entry and ends with
  // the node. declared_total is the sum of the entry sizes that the node
  // header declares.
  [[nodiscard]] Outcome Walk(std::span<const BYTE> first, DWORD declared_total);

 private:
  const Messages& messages_;
  bool recover_;
  std::vector<IndexEntryView>& entries_;
  std::vector<std::vector<BYTE>>& realigned_;

  // The state of the current Walk().
  DWORD declared_total_{0};
  // Sum of the sizes of the entries taken so far.
  DWORD total_{0};
  // False once an entry had a defect that recovery let pass.
  bool clean_{true};
  // Set when a defect ended the walk and the node MUST be rejected whole.
  bool rejected_{false};

  // Logs a defect. Returns true when the node MUST be rejected whole, after it
  // emptied the entries.
  [[nodiscard]] bool RejectOnDefect(std::string_view defect);

  // The header of the entry at the start of cur, once its size fits both the
  // bytes left and the declared total. Else nothing: the walk ends there, and
  // rejected_ tells whether the node is rejected.
  [[nodiscard]] std::optional<Data::IndexEntry>
      BoundedHeader(std::span<const BYTE> cur);

  // Checks the entry at the start of cur and appends its view to the entries.
  // False when a defect rejects the node whole.
  [[nodiscard]] bool Append(std::span<const BYTE> cur,
                            const Data::IndexEntry& head);
};  // IndexEntryWalker

}  // namespace NTFS_BROWSER_NAMESPACE::Attr
