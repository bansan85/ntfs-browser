#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>

#include "alloc/claim-inventory.h"
#include "mft/slot-scan.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Mft {

// The root directory's record number, fixed by NTFS.
inline constexpr ULONGLONG root_record = static_cast<ULONGLONG>(Mft::Idx::Root);

// The state of a tree: its entries and how they link. The scan and the link are
// in tree.cpp and tree-link.cpp, the journal in tree-journal.cpp and the paths
// in tree-paths.cpp.
class MftTree::Impl {
 public:
  std::vector<Mft::MftTree::Entry> entries;
  // Index in entries of each kept record, by record number, no_entry for the
  // others. It ends after the last record it holds. A record that would grow
  // it past what the entries justify sits in sparse_entries instead.
  std::vector<size_t> entry_of_record;
  std::unordered_map<ULONGLONG, size_t> sparse_entries;
  // The records filed under each entry, through a valid parent reference:
  // those of entries[i] are children_records[children_first[i]] up to
  // children_first[i + 1]. A root whose record is lost has its own list.
  std::vector<size_t> children_first;
  std::vector<ULONGLONG> children_records;
  std::vector<ULONGLONG> lost_root_children;
  // Parallel to entries_.
  std::vector<bool> reachable;
  Mft::MftTree::ScanStats stats;
  // The files only the journal names, ordered by record, then sequence.
  std::vector<Mft::MftTree::JournalOnlyEntry> journal_only;
  // The cluster accounts of the concerned entries, ordered by record. Empty
  // unless the scan accounted for clusters.
  std::vector<std::pair<ULONGLONG, Alloc::ClusterAccount>> accounts;

  template <Cache::Strategy S>
  void Scan(const NtfsVolume<S>& volume,
            const Mft::MftTree::ScanOptions& options);
  // Reads the slots into entries, indexes them and accounts for clusters.
  // Returns the clusters of $J when the scan met the $UsnJrnl record, or
  // nothing when a run told to stop after a phase ended the scan.
  template <Cache::Strategy S>
  std::optional<std::vector<Alloc::Extent>>
      ReadEntries(const NtfsVolume<S>& volume, const SlotScanInput& input);
  // Gives the concerned entries their ClusterAccount, from the claims of the
  // whole scan.
  template <Cache::Strategy S>
  void AccountClusters(const NtfsVolume<S>& volume,
                       const Mft::MftTree::ScanOptions& options,
                       Alloc::ClaimInventory& inventory);
  // Announces the clusters of $J to the reader, in the order the loader of the
  // journal reads them. Returns whether it announced any.
  template <Cache::Strategy S>
  bool AnnounceJournal(const NtfsVolume<S>& volume,
                       const std::vector<Alloc::Extent>& extents) const;
  void Link();
  // Files every entry under the parents its valid names give.
  void FileChildren();
  // Records that entries[index] holds record.
  void IndexEntry(ULONGLONG record, size_t index);
  // Index in entries of record, or nothing when the scan did not keep it.
  [[nodiscard]] std::optional<size_t> EntryIndex(ULONGLONG record) const;
  // announced says whether the scan announced the journal to the reader.
  template <Cache::Strategy S>
  void ApplyJournalIfComplete(const NtfsVolume<S>& volume, bool announced);
  void LogScanSummary(bool annotated) const;
  void LogClusterSummary() const;
  void LogJournalSummary() const;
  // Gives each entry the journal names its UsnInfo, and, with include_deleted,
  // lists the files it names that no entry accounts for.
  void ApplyJournal(const Usn::Journal& journal, bool include_deleted);
  [[nodiscard]] const Mft::MftTree::Entry* Find(ULONGLONG record) const;
  [[nodiscard]] std::span<const ULONGLONG> Children(ULONGLONG dir_record) const;
  [[nodiscard]] bool IsValidParent(const Mft::MftTree::Entry& child,
                                   const Mft::MftTree::Name& name) const;
  // Whether a parent reference names a directory the scan found, under the
  // same rules as IsValidParent(), minus the case of a record filed under
  // itself.
  [[nodiscard]] bool NamesDirectory(ULONGLONG parent_record,
                                    WORD parent_sequence) const;
  [[nodiscard]] std::wstring
      PathThrough(const Mft::MftTree::Entry& entry,
                  const Mft::MftTree::Name& name,
                  std::optional<ULONGLONG>* lost_ancestor) const;

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
      PathOfDirectory(const Mft::MftTree::Entry& directory,
                      const Mft::MftTree::Name& primary) const;
  // Climbs from primary, the primary name of directory, through the primary
  // names of its ancestors. own is the memo entry of directory, being built.
  // Each directory it passes gets a memo entry of its own, being built.
  [[nodiscard]] DirectoryChain ClimbChain(const Mft::MftTree::Entry& directory,
                                          const Mft::MftTree::Name& primary,
                                          DirectoryPath& own) const;
  // Settles a chain that loops: each directory is walked whole, and sees the
  // loop close at itself. Returns the path of the first.
  [[nodiscard]] DirectoryPath SettleLooped(const DirectoryChain& chain) const;
  // Settles a chain that ends: the names join from the top down, onto the path
  // of its known ancestor. Returns the path of the first directory.
  [[nodiscard]] DirectoryPath SettleChain(const DirectoryChain& chain) const;
  // Remembers item's path, or forgets item when the memo cannot hold it.
  void Remember(const PendingDirectory& item, const std::wstring& item_path,
                const std::optional<ULONGLONG>& item_lost) const;
  // Whether a path of length units may join the memo.
  [[nodiscard]] bool CanMemoize(size_t length) const noexcept;
  [[nodiscard]] std::wstring WalkPath(const Mft::MftTree::Entry& entry,
                                      const Mft::MftTree::Name& name,
                                      std::optional<ULONGLONG>& lost) const;

  // The directories PathThrough() went through, by record.
  mutable std::unordered_map<ULONGLONG, DirectoryPath> directory_paths_;
  // Units of path directory_paths_ holds.
  mutable size_t memoized_path_units_{0};
};

}  // namespace Mft
}  // namespace NTFS_BROWSER_NAMESPACE
