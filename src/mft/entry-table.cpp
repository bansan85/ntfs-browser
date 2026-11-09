#include "mft/entry-table.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "mft/file-reference.h"
#include "mft/tree.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// The entry index of a record the scan did not keep.
constexpr size_t no_entry = std::numeric_limits<size_t>::max();

// Records the dense index of a tree covers whatever its entries: 2^20 slots
// are 8 MiB, little next to the entries of a volume that large.
constexpr size_t dense_index_floor = size_t{1} << 20;

// Records the dense index covers per kept entry beyond the floor. Even a $MFT
// with seven free slots in eight stays dense, and a forged one far sparser
// falls back to the map instead of growing the index.
constexpr size_t dense_index_ratio = 8;

}  // namespace

std::vector<Mft::MftTree::Entry>& EntryTable::Entries() noexcept {
  return entries_;
}

const std::vector<Mft::MftTree::Entry>& EntryTable::Entries() const noexcept {
  return entries_;
}

void EntryTable::IndexAll() {
  for (size_t index = 0; index < entries_.size(); index++) {
    // index < entries_.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    Index(entries_[index].record, index);
  }
}

void EntryTable::Index(ULONGLONG record, size_t index) {
  const ULONGLONG dense_limit =
      (std::max)(static_cast<ULONGLONG>(dense_index_floor),
                 static_cast<ULONGLONG>(dense_index_ratio) * entries_.size());
  if (record >= dense_limit) {
    sparse_entries_.emplace(record, index);
    return;
  }
  // record < dense_limit, which is a size_t.
  const auto slot = static_cast<size_t>(record);
  if (slot >= entry_of_record_.size()) {
    entry_of_record_.resize(slot + 1, no_entry);
  }
  // The resize above makes slot an index of entry_of_record_.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  entry_of_record_[slot] = index;
}

std::optional<size_t> EntryTable::IndexOf(ULONGLONG record) const {
  if (record < entry_of_record_.size()) {
    // record < entry_of_record_.size() was checked above.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const size_t index = entry_of_record_[static_cast<size_t>(record)];
    if (index != no_entry) {
      return index;
    }
  }
  if (sparse_entries_.empty()) {
    return std::nullopt;
  }
  const auto found = sparse_entries_.find(record);
  if (found == sparse_entries_.end()) {
    return std::nullopt;
  }
  return found->second;
}

const Mft::MftTree::Entry* EntryTable::Find(ULONGLONG record) const {
  const std::optional<size_t> index = IndexOf(record);
  // IndexOf() gives an index of entries_.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return index ? &entries_[*index] : nullptr;
}

bool EntryTable::IsValidParent(const Mft::MftTree::Entry& child,
                               const Mft::MftTree::Name& name) const {
  // Only the root directory is filed under itself.
  if (name.parent_record == child.record) {
    return child.record == root_record;
  }

  return NamesDirectory(name.parent_record, name.parent_sequence);
}

bool EntryTable::NamesDirectory(ULONGLONG parent_record,
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

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
