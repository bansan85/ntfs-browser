#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

#include "mft/entry-table.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

// The name a record's path goes through by default: the first non-DOS name
// with a valid parent, else the first non-DOS name, else the first name. Null
// when the entry has no name.
[[nodiscard]] const Mft::MftTree::Name*
    PrimaryName(const Mft::MftTree::Entry& entry) noexcept;

// Builds the paths of the entries of a table. It remembers the path of each
// directory it builds, within a budget, so the files of one directory share
// one walk.
class PathBuilder {
 public:
  // Joins name and its ancestors' primary names up to the root, or up to the
  // record where the chain of valid parent references breaks. A file's path is
  // its parent's, walked once for all its files, plus its name: a file is never
  // a valid parent, so it cannot sit on the chain above it. lost_ancestor, when
  // not null, receives that record, or nothing when the chain reaches the root.
  [[nodiscard]] std::wstring
      PathThrough(const EntryTable& table, const Mft::MftTree::Entry& entry,
                  const Mft::MftTree::Name& name,
                  std::optional<ULONGLONG>* lost_ancestor);

 private:
  // The path of a directory through its primary name, as PathThrough() builds
  // it, and the record where its chain breaks.
  struct DirectoryPath {
    std::wstring path;
    std::optional<ULONGLONG> lost;
    // Set while PathOfDirectory() builds the path. A chain of parents that
    // meets it has looped.
    bool building{false};
  };

  // One directory whose path PathOfDirectory() builds.
  struct PendingDirectory {
    const Mft::MftTree::Entry* entry;
    const Mft::MftTree::Name* primary;
    DirectoryPath* path;
  };

  // The directories from one up to the first whose path is known, or to the
  // root, or to the record where the chain of parents breaks.
  struct DirectoryChain {
    // The directory first, then its ancestors.
    std::vector<PendingDirectory> pending;
    // The ancestor whose path is known, when the chain reached one.
    const DirectoryPath* known{nullptr};
    // The record where the chain breaks.
    std::optional<ULONGLONG> lost;
    // The chain met a directory that is being built: it loops.
    bool looped{false};
  };

  [[nodiscard]] DirectoryPath
      PathOfDirectory(const EntryTable& table,
                      const Mft::MftTree::Entry& directory,
                      const Mft::MftTree::Name& primary);
  // Climbs from primary, the primary name of directory, through the primary
  // names of its ancestors. own is the memo entry of directory, being built.
  // Each directory it passes gets a memo entry of its own, being built.
  [[nodiscard]] DirectoryChain ClimbChain(const EntryTable& table,
                                          const Mft::MftTree::Entry& directory,
                                          const Mft::MftTree::Name& primary,
                                          DirectoryPath& own);
  // Settles a chain that loops: each directory is walked whole, and sees the
  // loop close at itself. Returns the path of the first.
  [[nodiscard]] DirectoryPath SettleLooped(const EntryTable& table,
                                           const DirectoryChain& chain);
  // Settles a chain that ends: the names join from the top down, onto the path
  // of its known ancestor. Returns the path of the first directory.
  [[nodiscard]] DirectoryPath SettleChain(const DirectoryChain& chain);
  // Remembers item's path, or forgets item when the memo cannot hold it.
  void Remember(const PendingDirectory& item, const std::wstring& item_path,
                const std::optional<ULONGLONG>& item_lost);
  // Whether a path of length units may join the memo.
  [[nodiscard]] bool CanMemoize(size_t length) const noexcept;
  // Walks from name up the primary names of entry's ancestors, see
  // PathThrough(). entry is not the root. lost receives the record where the
  // chain breaks, if it does.
  [[nodiscard]] static std::wstring WalkPath(const EntryTable& table,
                                             const Mft::MftTree::Entry& entry,
                                             const Mft::MftTree::Name& name,
                                             std::optional<ULONGLONG>& lost);

  // The directories PathThrough() went through, by record.
  std::unordered_map<ULONGLONG, DirectoryPath> directory_paths_;
  // Units of path directory_paths_ holds.
  size_t memoized_path_units_{0};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
