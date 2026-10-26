#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/record/carved-name.h>

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

template <Cache::Strategy S>
class AttrBase;

}  // namespace Attr

namespace UpCase {

class Table;

}  // namespace UpCase
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

class IndexEntry;
class IndexEntryView;

namespace Attr {

template <Cache::Strategy S>
class AttrIndexAlloc;

}  // namespace Attr

namespace Io {

// How the recovery scan walks one $INDEX_ALLOCATION attribute.
struct OrphanScanPlan {
  // VCN step from one index block to the next.
  ULONGLONG clusters_per_block;
  // Blocks the attribute declares.
  ULONGLONG declared_blocks;
  // Blocks the scan visits.
  ULONGLONG scan_limit;
  // True when the scan visits fewer blocks than declared.
  bool capped;
};

// One name lookup down a B+ tree: the name, the volume's collation, and the
// index block VCNs the walk has already visited. The B+ tree collates through
// $UpCase, so two names that differ only by case are equal keys, and both can
// sit in one directory (POSIX names, a case-sensitive directory). A lookup
// takes the entry spelled exactly as file_name when there is one. Otherwise it
// takes the first equal entry in index order, which is kept in fallback.
struct IndexSearch {
  std::wstring_view file_name;
  const UpCase::Table& upcase;
  std::unordered_set<ULONGLONG> visited_vcns;
  // The first visible entry equal to file_name that is not spelled like it.
  std::optional<IndexEntry> fallback;
  // Whether that entry sits in the $INDEX_ROOT: only the debug line differs.
  bool fallback_at_root{false};
  // Whether an entry equal to file_name was met, hidden or not. Past the
  // sub-node of the next greater entry, no twin of it can sit.
  bool met_equal{false};
};

template <Cache::Strategy S>
class EntryProber;

// Where a walk of the entries sends them, and how it finds out which ones to
// hide.
template <Cache::Strategy S>
struct WalkTarget {
  using SubentryCallback = typename FileRecord<S>::SubentryCallback;
  using StandingCallback = typename FileRecord<S>::StandingCallback;

  // The callback of the default walk, which probes each entry as it comes.
  // Null in the batched walk.
  const SubentryCallback* plain{nullptr};
  // The callback of the batched walk. Null in the default walk.
  const StandingCallback* standing{nullptr};
  void* context{nullptr};
  // Probes the entries of each node together. Set exactly when standing is.
  EntryProber<S>* prober{nullptr};
};

// Walks the B+ tree of one directory: its $INDEX_ROOT and $INDEX_ALLOCATION
// blocks. Short-lived: built over a FileRecord for one call, never stored.
template <Cache::Strategy S>
class DirectoryIndex {
 public:
  using SubentryCallback = typename FileRecord<S>::SubentryCallback;
  using StandingCallback = typename FileRecord<S>::StandingCallback;

  explicit DirectoryIndex(const FileRecord<S>& record) noexcept;

  // Calls se_call_back for every named entry, in collation order, then for
  // the orphaned blocks when recovery is on.
  void Traverse(const SubentryCallback& se_call_back, void* context) const;
  // Traverse() with the probes of each index node batched, and the standing of
  // the named record handed to the callback. See
  // FileRecord<S>::TraverseSubEntriesWithStanding().
  void TraverseWithStanding(const StandingCallback& se_call_back,
                            void* context) const;
  // Finds file_name. upcase is the volume's collation, loaded by the caller.
  [[nodiscard]] std::optional<IndexEntry>
      Find(std::wstring_view file_name, const UpCase::Table& upcase) const;
  // The names the unused space of the $INDEX_ALLOCATION blocks still holds,
  // see FileRecord<S>::CarveIndexSlackNames(). Times are UTC, as stored.
  [[nodiscard]] std::vector<Record::CarvedName> CarveSlackNames() const;

 private:
  // The record whose index this walks. It MUST outlive this object.
  const FileRecord<S>& record_;

  [[nodiscard]] static const std::vector<IndexEntryView>*
      FileNameIndexRootEntries(const Attr::AttrBase<S>& attr);
  [[nodiscard]] std::optional<IndexEntry>
      FindSubEntryInOrder(std::wstring_view file_name,
                          const UpCase::Table& upcase) const;
  [[nodiscard]] std::optional<IndexEntry>
      VisitIndexBlock(ULONGLONG vcn, IndexSearch& search, size_t depth) const;
  [[nodiscard]] std::optional<IndexEntry>
      SearchIndexEntries(const std::vector<IndexEntryView>& entries,
                         IndexSearch& search, size_t child_depth,
                         bool at_root) const;
  void Walk(WalkTarget<S>& target) const;
  void Deliver(const IndexEntryView& entry, WalkTarget<S>& target) const;
  void TraverseSubNode(ULONGLONG vcn, WalkTarget<S>& target,
                       std::unordered_set<ULONGLONG>& visited_vcns,
                       size_t depth) const;
  void ScanOrphanedIndexBlocks(
      WalkTarget<S>& target, std::unordered_set<ULONGLONG>& visited_vcns) const;
  [[nodiscard]] OrphanScanPlan
      PlanOrphanScan(const Attr::AttrIndexAlloc<S>& alloc) const;
  void ReportOrphanedBlock(Attr::AttrIndexAlloc<S>& alloc, ULONGLONG vcn,
                           WalkTarget<S>& target) const;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
