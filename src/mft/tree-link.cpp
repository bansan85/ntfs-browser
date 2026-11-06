#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

#include "mft/file-reference.h"
#include "mft/tree-impl.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// The entry index of a record the scan did not keep.
constexpr size_t no_entry = std::numeric_limits<size_t>::max();

// Records the dense index of a tree covers whatever its entries: 2^20 slots
// are 8 MiB, little next to the entries of a volume that large.
constexpr size_t dense_index_floor = size_t{1} << 20;

// Records the dense index covers per kept entry beyond the floor. Even a
// $MFT with seven free slots in eight stays dense, and a forged one far
// sparser falls back to the map instead of growing the index.
constexpr size_t dense_index_ratio = 8;

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

}  // namespace

// Validates every name's parent reference, files each record under the
// parents its valid names give, then marks what the root reaches.
void Mft::MftTree::Impl::Link() {
  for (Mft::MftTree::Entry& entry : entries) {
    for (Mft::MftTree::Name& name : entry.names) {
      name.parent_valid = IsValidParent(entry, name);
    }
  }
  FileChildren();

  reachable.assign(entries.size(), false);
  if (const std::optional<size_t> root = EntryIndex(root_record)) {
    // EntryIndex() gives an index of entries; reachable is as long.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    reachable[*root] = true;
  }

  std::vector<ULONGLONG> pending{root_record};
  while (!pending.empty()) {
    const ULONGLONG dir = pending.back();
    pending.pop_back();
    for (const ULONGLONG child : Children(dir)) {
      // Every child is an entry.
      const size_t index = EntryIndex(child).value();
      // EntryIndex() gives an index of entries; reachable is as long.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      auto flag = reachable[index];
      if (!flag) {
        flag = true;
        pending.push_back(child);
      }
    }
  }

  stats.unreachable = static_cast<ULONGLONG>(
      std::count(reachable.begin(), reachable.end(), false));
}

// Counts the children of each entry, then lays them out in children_records,
// each entry's run in the order of entries.
void Mft::MftTree::Impl::FileChildren() {
  children_first.assign(entries.size() + 1, 0);
  for (const Mft::MftTree::Entry& entry : entries) {
    for (size_t i = 0; i < entry.names.size(); i++) {
      if (!FilesUnderParent(entry, i)) {
        continue;
      }
      // i < entry.names.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const ULONGLONG parent = entry.names[i].parent_record;
      if (const std::optional<size_t> index = EntryIndex(parent)) {
        // EntryIndex() gives an index below entries.size().
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        children_first[*index + 1]++;
      } else {
        // Only the root is a valid parent without an entry.
        lost_root_children.push_back(entry.record);
      }
    }
  }
  std::partial_sum(children_first.begin(), children_first.end(),
                   children_first.begin());

  children_records.resize(children_first.back());
  std::vector<size_t> next(children_first.begin(), children_first.end() - 1);
  for (const Mft::MftTree::Entry& entry : entries) {
    for (size_t i = 0; i < entry.names.size(); i++) {
      if (!FilesUnderParent(entry, i)) {
        continue;
      }
      // i < entry.names.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const ULONGLONG parent = entry.names[i].parent_record;
      if (const std::optional<size_t> index = EntryIndex(parent)) {
        // next is as long as entries, and its values index children_records,
        // which the counts above sized.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        children_records[next[*index]++] = entry.record;
      }
    }
  }
}

void Mft::MftTree::Impl::IndexEntry(ULONGLONG record, size_t index) {
  const ULONGLONG dense_limit =
      (std::max)(static_cast<ULONGLONG>(dense_index_floor),
                 static_cast<ULONGLONG>(dense_index_ratio) * entries.size());
  if (record >= dense_limit) {
    sparse_entries.emplace(record, index);
    return;
  }
  // record < dense_limit, which is a size_t.
  const auto slot = static_cast<size_t>(record);
  if (slot >= entry_of_record.size()) {
    entry_of_record.resize(slot + 1, no_entry);
  }
  // The resize above makes slot an index of entry_of_record.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  entry_of_record[slot] = index;
}

std::optional<size_t> Mft::MftTree::Impl::EntryIndex(ULONGLONG record) const {
  if (record < entry_of_record.size()) {
    // record < entry_of_record.size() was checked above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const size_t index = entry_of_record[static_cast<size_t>(record)];
    if (index != no_entry) {
      return index;
    }
  }
  if (sparse_entries.empty()) {
    return std::nullopt;
  }
  const auto found = sparse_entries.find(record);
  if (found == sparse_entries.end()) {
    return std::nullopt;
  }
  return found->second;
}

// Whether name's parent reference, read from child's record, names a
// directory the scan found. See the rules in mft/tree.h.
bool Mft::MftTree::Impl::IsValidParent(const Mft::MftTree::Entry& child,
                                       const Mft::MftTree::Name& name) const {
  // Only the root directory is filed under itself.
  if (name.parent_record == child.record) {
    return child.record == root_record;
  }

  return NamesDirectory(name.parent_record, name.parent_sequence);
}

bool Mft::MftTree::Impl::NamesDirectory(ULONGLONG parent_record,
                                        WORD parent_sequence) const {
  const Mft::MftTree::Entry* parent = Find(parent_record);
  if (parent == nullptr) {
    return parent_record == root_record;
  }
  if (!parent->directory) {
    return false;
  }

  // The parent is the same one the reference was written for: unchecked, still
  // the same generation, or the generation NTFS freed right after.
  return Mft::IsSameRecordGeneration(parent_sequence, parent->sequence,
                                     parent->in_use);
}

const std::vector<Mft::MftTree::Entry>& Mft::MftTree::Entries() const noexcept {
  return impl_->entries;
}

// The lookup behind MftTree::Find().
const Mft::MftTree::Entry* Mft::MftTree::Impl::Find(ULONGLONG record) const {
  const std::optional<size_t> index = EntryIndex(record);
  // EntryIndex() gives an index of entries.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return index ? &entries[*index] : nullptr;
}

// The lookup behind MftTree::Children().
std::span<const ULONGLONG>
    Mft::MftTree::Impl::Children(ULONGLONG dir_record) const {
  const std::optional<size_t> index = EntryIndex(dir_record);
  if (!index) {
    return dir_record == root_record
               ? std::span<const ULONGLONG>{lost_root_children}
               : std::span<const ULONGLONG>{};
  }
  // children_first holds one more value than there are entries.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const size_t first = children_first[*index];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const size_t last = children_first[*index + 1];
  return std::span<const ULONGLONG>{children_records}.subspan(first,
                                                              last - first);
}

const Mft::MftTree::Entry* Mft::MftTree::Find(ULONGLONG record) const {
  return impl_->Find(record);
}

std::span<const ULONGLONG> Mft::MftTree::Children(ULONGLONG dir_record) const {
  return impl_->Children(dir_record);
}

bool Mft::MftTree::IsReachable(ULONGLONG record) const {
  const std::optional<size_t> index = impl_->EntryIndex(record);
  if (!index) {
    return false;
  }
  // EntryIndex() gives an index of entries; reachable is as long.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return impl_->reachable[*index];
}

std::span<const Mft::MftTree::JournalOnlyEntry>
    Mft::MftTree::JournalOnly() const noexcept {
  return impl_->journal_only;
}

const Alloc::ClusterAccount*
    Mft::MftTree::ClusterAccountOf(ULONGLONG record) const {
  const auto found =
      std::ranges::lower_bound(impl_->accounts, record, {},
                               &decltype(impl_->accounts)::value_type::first);
  return found != impl_->accounts.end() && found->first == record
             ? &found->second
             : nullptr;
}

const Mft::MftTree::ScanStats& Mft::MftTree::Stats() const noexcept {
  return impl_->stats;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
