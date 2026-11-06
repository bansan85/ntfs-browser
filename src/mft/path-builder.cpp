#include "mft/path-builder.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

#include "mft/entry-table.h"
#include "mft/tree-testing.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// Units of path the directory path memo holds at most, in all. A deep chain of
// directories has paths that grow with its depth, so remembering each one costs
// memory that grows with its square. 2^26 units is 128 MiB of UTF-16, far above
// what a real volume of millions of directories needs.
constexpr size_t default_max_memoized_path_units = size_t{1} << 26;

// The budget a test asked for. Unset means the default.
std::optional<size_t> max_memoized_path_units_override;

// The most units of path any tree of this thread has held in its memo, since a
// test last reset it. Read by tests only.
thread_local size_t peak_memoized_path_units = 0;

// Units a directory path can have and still be memoized. Past a normal path
// (MAX_PATH is 260), a path is rebuilt each time it is asked for: that costs
// the length of the path, which the caller pays for anyway.
constexpr size_t max_memoized_path_length = 1024;

}  // namespace

const Mft::MftTree::Name*
    PrimaryName(const Mft::MftTree::Entry& entry) noexcept {
  const Mft::MftTree::Name* fallback = nullptr;
  for (const Mft::MftTree::Name& name : entry.names) {
    if (name.dos_only) {
      continue;
    }
    if (name.parent_valid) {
      return &name;
    }
    if (fallback == nullptr) {
      fallback = &name;
    }
  }
  if (fallback != nullptr) {
    return fallback;
  }
  return entry.names.empty() ? nullptr : &entry.names.front();
}

void SetMaxMemoizedPathUnitsForTests(std::optional<size_t> units) {
  max_memoized_path_units_override = units;
}

size_t PeakMemoizedPathUnitsForTests(bool reset) noexcept {
  const size_t peak = peak_memoized_path_units;
  if (reset) {
    peak_memoized_path_units = 0;
  }
  return peak;
}

std::wstring PathBuilder::PathThrough(const EntryTable& table,
                                      const Mft::MftTree::Entry& entry,
                                      const Mft::MftTree::Name& name,
                                      std::optional<ULONGLONG>* lost_ancestor) {
  if (lost_ancestor != nullptr) {
    lost_ancestor->reset();
  }
  if (entry.record == root_record) {
    return L"\\";
  }

  // A valid parent other than the root is a directory the scan found.
  const Mft::MftTree::Entry* parent =
      name.parent_valid && name.parent_record != root_record
          ? table.Find(name.parent_record)
          : nullptr;
  const Mft::MftTree::Name* parent_name =
      parent != nullptr ? PrimaryName(*parent) : nullptr;

  std::wstring path;
  std::optional<ULONGLONG> lost;
  if (entry.directory && &name == PrimaryName(entry)) {
    // The path its files reuse.
    DirectoryPath own = PathOfDirectory(table, entry, name);
    path = std::move(own.path);
    lost = own.lost;
  } else if (!entry.directory && parent_name != nullptr) {
    DirectoryPath directory = PathOfDirectory(table, *parent, *parent_name);
    path = std::move(directory.path);
    path.reserve(path.size() + 1 + name.name.size());
    path += L'\\';
    path += name.name;
    lost = directory.lost;
  } else {
    path = WalkPath(table, entry, name, lost);
  }

  if (lost_ancestor != nullptr) {
    *lost_ancestor = lost;
  }
  return path;
}

// The path of directory through primary, its primary name, remembered once
// built unless it is long or the memo is full: the path of the first ancestor
// already known, or of the root or the record where the chain breaks, joined to
// the names below it. The directories of a chain that loops are walked whole,
// one by one: each sees the loop close at itself.
PathBuilder::DirectoryPath
    PathBuilder::PathOfDirectory(const EntryTable& table,
                                 const Mft::MftTree::Entry& directory,
                                 const Mft::MftTree::Name& primary) {
  const auto [found, added] = directory_paths_.try_emplace(directory.record);
  DirectoryPath& own = found->second;
  if (!added && !own.building) {
    return own;
  }
  if (!added) {
    // Left building by a call that threw.
    own.path = WalkPath(table, directory, primary, own.lost);
    own.building = false;
    return own;
  }

  own.building = true;
  const DirectoryChain chain = ClimbChain(table, directory, primary, own);
  return chain.looped ? SettleLooped(table, chain) : SettleChain(chain);
}

PathBuilder::DirectoryChain PathBuilder::ClimbChain(
    const EntryTable& table, const Mft::MftTree::Entry& directory,
    const Mft::MftTree::Name& primary, DirectoryPath& own) {
  DirectoryChain chain;
  chain.pending.push_back({&directory, &primary, &own});
  const Mft::MftTree::Name* name = &primary;
  while (true) {
    const ULONGLONG parent = name->parent_record;
    if (!name->parent_valid) {
      chain.lost = parent;
      break;
    }
    if (parent == root_record) {
      break;
    }
    // parent_valid comes from IsValidParent(), which finds parent in the scan.
    const Mft::MftTree::Entry* parent_entry = table.Find(parent);
    assert(parent_entry != nullptr);
    const Mft::MftTree::Name* parent_name = PrimaryName(*parent_entry);
    if (parent_name == nullptr) {
      chain.lost = parent;
      break;
    }
    const auto [parent_found, parent_added] =
        directory_paths_.try_emplace(parent);
    if (!parent_added) {
      chain.looped = parent_found->second.building;
      chain.known = &parent_found->second;
      break;
    }
    parent_found->second.building = true;
    chain.pending.push_back({parent_entry, parent_name, &parent_found->second});
    name = parent_name;
  }
  return chain;
}

PathBuilder::DirectoryPath
    PathBuilder::SettleLooped(const EntryTable& table,
                              const DirectoryChain& chain) {
  // What the caller gets, taken before Remember() can forget the entry of the
  // first directory.
  DirectoryPath result;
  for (const PendingDirectory& item : chain.pending) {
    std::optional<ULONGLONG> item_lost;
    const std::wstring item_path =
        WalkPath(table, *item.entry, *item.primary, item_lost);
    if (&item == &chain.pending.front()) {
      result.path = item_path;
      result.lost = item_lost;
    }
    Remember(item, item_path, item_lost);
  }
  return result;
}

PathBuilder::DirectoryPath
    PathBuilder::SettleChain(const DirectoryChain& chain) {
  std::wstring path =
      chain.known != nullptr ? chain.known->path : std::wstring{};
  std::optional<ULONGLONG> lost = chain.lost;
  if (chain.known != nullptr) {
    lost = chain.known->lost;
  }
  // The names join as WalkPath() joins them, from the top down. The first
  // directory comes last, and its path is the one the caller gets.
  for (const PendingDirectory& item : chain.pending | std::views::reverse) {
    if (!path.empty() || !lost) {
      path += L'\\';
    }
    path += item.primary->name;
    Remember(item, path, lost);
  }
  DirectoryPath result;
  result.path = std::move(path);
  result.lost = lost;
  return result;
}

void PathBuilder::Remember(const PendingDirectory& item,
                           const std::wstring& item_path,
                           const std::optional<ULONGLONG>& item_lost) {
  if (!CanMemoize(item_path.size())) {
    directory_paths_.erase(item.entry->record);
    return;
  }
  memoized_path_units_ += item_path.size();
  peak_memoized_path_units =
      (std::max)(peak_memoized_path_units, memoized_path_units_);
  item.path->path = item_path;
  item.path->lost = item_lost;
  item.path->building = false;
}

bool PathBuilder::CanMemoize(size_t length) const noexcept {
  // A short path, and room left in the budget for it.
  return length <= max_memoized_path_length &&
         memoized_path_units_ + length <=
             max_memoized_path_units_override.value_or(
                 default_max_memoized_path_units);
}

std::wstring PathBuilder::WalkPath(const EntryTable& table,
                                   const Mft::MftTree::Entry& entry,
                                   const Mft::MftTree::Name& name,
                                   std::optional<ULONGLONG>& lost) {
  std::vector<const std::wstring*> parts;
  // Guards against a forged chain of parents that loops back on itself.
  std::unordered_set<ULONGLONG> visited{entry.record};
  const Mft::MftTree::Name* current = &name;
  lost.reset();

  while (true) {
    parts.push_back(&current->name);
    const ULONGLONG parent = current->parent_record;
    if (!current->parent_valid || !visited.insert(parent).second) {
      lost = parent;
      break;
    }
    if (parent == root_record) {
      break;
    }

    // parent_valid comes from IsValidParent(), which finds parent in the scan.
    const Mft::MftTree::Entry* parent_entry = table.Find(parent);
    assert(parent_entry != nullptr);
    current = PrimaryName(*parent_entry);
    if (current == nullptr) {
      lost = parent;
      break;
    }
  }

  std::wstring path;
  for (const auto* const part : parts | std::views::reverse) {
    if (!path.empty() || !lost) {
      path += L'\\';
    }
    path += *part;
  }
  return path;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
