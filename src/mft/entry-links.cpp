#include "mft/entry-links.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "mft/entry-table.h"
#include "mft/tree.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// Whether entry.names[index] files entry under its parent. Only a valid,
// non-DOS name files a record, the root is filed under nothing, and two hard
// links in one directory file the record there once.
bool FilesUnderParent(const Mft::MftTree::Entry& entry, size_t index) {
  const auto files = [&entry](const Mft::MftTree::Name& name) {
    return name.parent_valid && !name.dos_only && entry.record != root_record;
  };
  const Mft::MftTree::Name& name = entry.names.at(index);
  if (!files(name)) {
    return false;
  }
  return std::ranges::none_of(
      entry.names.begin(),
      std::next(entry.names.begin(), gsl::narrow<std::ptrdiff_t>(index)),
      [&](const Mft::MftTree::Name& earlier) {
        return files(earlier) && earlier.parent_record == name.parent_record;
      });
}

// Sets parent_valid of every name of the table, by the rules of MftTree.
void ValidateParents(EntryTable& table) {
  for (Mft::MftTree::Entry& entry : table.Entries()) {
    for (Mft::MftTree::Name& name : entry.names) {
      name.parent_valid = table.IsValidParent(entry, name);
    }
  }
}

}  // namespace

ULONGLONG EntryLinks::Link(EntryTable& table) {
  ValidateParents(table);
  FileChildren(table);
  MarkReachable(table);
  return static_cast<ULONGLONG>(
      std::count(reachable_.begin(), reachable_.end(), false));
}

// Marks every entry the root reaches through the children lists.
void EntryLinks::MarkReachable(const EntryTable& table) {
  reachable_.assign(table.Entries().size(), false);
  if (const std::optional<size_t> root = table.IndexOf(root_record)) {
    // IndexOf() gives an index of the entries; reachable_ is as long.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    reachable_[*root] = true;
  }

  std::vector<ULONGLONG> pending{root_record};
  while (!pending.empty()) {
    const ULONGLONG dir = pending.back();
    pending.pop_back();
    for (const ULONGLONG child : Children(table, dir)) {
      // Every child is an entry.
      const size_t index = table.IndexOf(child).value();
      // IndexOf() gives an index of the entries; reachable_ is as long.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      auto flag = reachable_[index];
      if (!flag) {
        flag = true;
        pending.push_back(child);
      }
    }
  }
}

// Counts the children of each entry, then lays them out in children_records_,
// each entry's run in the order of the entries.
void EntryLinks::FileChildren(const EntryTable& table) {
  const std::vector<Mft::MftTree::Entry>& entries = table.Entries();
  children_first_.assign(entries.size() + 1, 0);
  for (const Mft::MftTree::Entry& entry : entries) {
    for (size_t i = 0; i < entry.names.size(); i++) {
      if (!FilesUnderParent(entry, i)) {
        continue;
      }
      // i < entry.names.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const ULONGLONG parent = entry.names[i].parent_record;
      if (const std::optional<size_t> index = table.IndexOf(parent)) {
        // IndexOf() gives an index below entries.size().
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        children_first_[*index + 1]++;
      } else {
        // Only the root is a valid parent without an entry.
        lost_root_children_.push_back(entry.record);
      }
    }
  }
  std::partial_sum(children_first_.begin(), children_first_.end(),
                   children_first_.begin());

  children_records_.resize(children_first_.back());
  std::vector<size_t> next(children_first_.begin(), children_first_.end() - 1);
  for (const Mft::MftTree::Entry& entry : entries) {
    for (size_t i = 0; i < entry.names.size(); i++) {
      if (!FilesUnderParent(entry, i)) {
        continue;
      }
      // i < entry.names.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const ULONGLONG parent = entry.names[i].parent_record;
      if (const std::optional<size_t> index = table.IndexOf(parent)) {
        // next is as long as entries, and its values index children_records_,
        // which the counts above sized.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        children_records_[next[*index]++] = entry.record;
      }
    }
  }
}

std::span<const ULONGLONG> EntryLinks::Children(const EntryTable& table,
                                                ULONGLONG dir_record) const {
  const std::optional<size_t> index = table.IndexOf(dir_record);
  if (!index) {
    return dir_record == root_record
               ? std::span<const ULONGLONG>{lost_root_children_}
               : std::span<const ULONGLONG>{};
  }
  // children_first_ holds one more value than there are entries.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const size_t first = children_first_[*index];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const size_t last = children_first_[*index + 1];
  return std::span<const ULONGLONG>{children_records_}.subspan(first,
                                                               last - first);
}

bool EntryLinks::IsReachable(size_t index) const noexcept {
  // The size is checked first.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return index < reachable_.size() && reachable_[index];
}

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
