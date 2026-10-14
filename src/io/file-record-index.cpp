#include "io/file-record-index.h"

#include <ntfs-browser/win-types.h>

#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>

#include "attr/index-alloc.h"
#include "attr/index-block.h"
#include "attr/index-root.h"
#include "attr/resident.h"
#include "io/file-record-attrs.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "upcase/upcase.h"

namespace NtfsBrowser::Io {

namespace {

// Chosen well above any real NTFS directory's B+ tree depth, but low enough
// to unwind long before a forged chain overflows the stack.
constexpr size_t max_index_block_depth = 64;

// Caps the orphan-block recovery scan: an attacker-controlled declared block
// count must not drive an unbounded number of ParseIndexBlock() calls. 65536
// blocks is already far past any real directory's index, so this only ever
// binds on a forged/damaged $INDEX_ALLOCATION.
constexpr size_t max_orphan_scan_blocks = 65536;

// Logs a name match found by SearchIndexEntries(). The root and the index
// blocks log under different names.
void LogIndexMatch(bool at_root) {
  if (at_root) {
    Log::Debug("FindSubEntry() found entry in Index Root");
  } else {
    Log::Debug("VisitIndexBlock() found entry in sub-node");
  }
}

// Number of whole index blocks that mappedClusters clusters hold. A range of
// clusters counts in bytes, not in clusters, since a block can be smaller than
// one. The byte count saturates instead of wrapping.
ULONGLONG MappedIndexBlocks(ULONGLONG mapped_clusters, DWORD cluster_size,
                            DWORD index_block_size) noexcept {
  const ULONGLONG mapped_bytes =
      (cluster_size != 0 &&
       mapped_clusters > std::numeric_limits<ULONGLONG>::max() / cluster_size)
          ? std::numeric_limits<ULONGLONG>::max()
          : mapped_clusters * cluster_size;
  return mapped_bytes / index_block_size;
}

// Step, in VCN units, from one index block to the next. A sub-node VCN (what
// visitedVcns holds, and what ParseIndexBlock() expects) is in clusters when a
// block spans a whole cluster or more, but in index_block_size units when a
// cluster is too big to hold one - the same conversion ParseIndexBlock() itself
// applies.
ULONGLONG ClustersPerIndexBlock(DWORD index_block_size,
                                DWORD cluster_size) noexcept {
  return (cluster_size != 0 && index_block_size >= cluster_size)
             ? index_block_size / cluster_size
             : 1;
}

// The directory an orphan scan reports entries for: its file reference when
// known, and the sequence number and liveness it has now.
struct OrphanOwner {
  std::optional<ULONGLONG> file_ref;
  WORD sequence;
  bool in_use;
};

// Reads the orphan scan's directory identity from record.
template <Cache::Strategy S>
OrphanOwner OwnerOf(const FileRecord<S>& record) {
  return {record.GetFileReference(), record.GetSequenceNumber(),
          !record.IsDeleted()};
}

// Decides whether an entry found in an orphaned block is reported. It MUST be
// filed under owner's directory. With a volume given, the record it names
// MUST also still be in use under the same sequence.
template <Cache::Strategy S>
bool IsOrphanEntryReportable(const IndexEntryView& entry,
                             const OrphanOwner& owner,
                             const NtfsVolume<S>* check_named_record_in) {
  // An orphaned block may hold a stale entry left over from a file
  // already deleted from this directory, or from an earlier directory
  // that used this record - only report one still filed under this very
  // directory. Same rule as MftTree: a freed directory keeps the entries
  // filed under its sequence from before NTFS bumped it.
  if (owner.file_ref &&
      (entry.GetParentReference() != *owner.file_ref ||
       !Mft::IsSameRecordGeneration(entry.GetParentSequenceNumber(),
                                    owner.sequence, owner.in_use))) {
    return false;
  }
  // With include_deleted off, also drop an entry whose named record is
  // itself freed, or was reused under a different sequence number.
  if (check_named_record_in != nullptr) {
    Io::FileRecord<S> named(*check_named_record_in);
    return named.ParseFileRecord(entry.GetFileReference()) &&
           !named.IsDeleted() &&
           named.GetSequenceNumber() == entry.GetSequenceNumber();
  }
  return true;
}

}  // namespace

template <Cache::Strategy S>
Io::DirectoryIndex<S>::DirectoryIndex(const FileRecord<S>& record) noexcept
    : record_(record) {}

// Traverse all sub directories and files contained
// Call user defined callback routine once found an entry
template <Cache::Strategy S>
void Io::DirectoryIndex<S>::Traverse(const SubentryCallback& se_call_back,
                                     void* context) const {
  assert(se_call_back);

  const bool recover = record_.GetVolume().GetOptions().recover_errors;

  // Start traversing from IndexRoot (B+ tree root node)

  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      record_.GetAttr(Attr::Type::IndexRoot);
  if (vec.empty()) {
    // No IndexRoot at all to start the normal walk from, but $INDEX_ALLOCATION
    // blocks may still exist and hold every entry.
    if (recover) {
      std::unordered_set<ULONGLONG> visited_vcns;
      ScanOrphanedIndexBlocks(se_call_back, context, visited_vcns);
    }
    return;
  }

  // Only a $INDEX_ROOT that indexes file names is walked.
  const auto* index_root =
      reinterpret_cast<const Attr::AttrIndexRoot<ResidentOf<S>, S>*>(
          vec.front().get());
  if (!index_root->IsFileName()) {
    return;
  }
  const std::vector<IndexEntryView>& all_ie = *index_root;

  std::unordered_set<ULONGLONG> visited_vcns;

  for (const IndexEntryView& index_entry : all_ie) {
    // Visit subnode first
    if (index_entry.IsSubNodePtr()) {
      TraverseSubNode(index_entry.GetSubNodeVCN(), se_call_back, context,
                      visited_vcns, 0);
    }

    if (index_entry.HasName()) {
      se_call_back(index_entry, context);
    }
  }

  if (recover) {
    ScanOrphanedIndexBlocks(se_call_back, context, visited_vcns);
  }
}

// Find a specific Filename from the B+ tree, then, if the name is missing and
// the volume has no $UpCase table, from every entry.
template <Cache::Strategy S>
std::optional<IndexEntry>
    Io::DirectoryIndex<S>::Find(std::wstring_view file_name,
                                const UpCase::Table& upcase) const {
  std::optional<IndexEntry> found = FindSubEntryInOrder(file_name, upcase);
  if (found || !upcase.IsBuiltIn()) {
    return found;
  }

  // The built-in mapping can disagree with the volume's own collation, and
  // the ordered search then stops at a leaf that is not the end of the
  // name's range. Look at every entry instead.
  Log::Debug("FindSubEntry() scans every entry: no $UpCase table");
  Traverse(
      [&](const IndexEntryView& index_entry, void*) {
        if (!found && index_entry.Compare(file_name, upcase) == 0) {
          found.emplace(index_entry);
        }
      },
      nullptr);
  return found;
}

// The entries of a file-name $INDEX_ROOT, or null for any other kind of index
// or for a strategy this library does not know.
template <Cache::Strategy S>
const std::vector<IndexEntryView>*
    Io::DirectoryIndex<S>::FileNameIndexRootEntries(
        const Attr::AttrBase<S>& attr) {
  if constexpr (S == Cache::Strategy::NoCache) {
    const auto* index_root = reinterpret_cast<const Attr::AttrIndexRoot<
        Attr::AttrResidentNoCache, Cache::Strategy::NoCache>*>(&attr);
    return index_root->IsFileName() ? index_root : nullptr;
  } else if constexpr (S == Cache::Strategy::FullCache) {
    const auto* index_root = reinterpret_cast<const Attr::AttrIndexRoot<
        Attr::AttrResidentFullCache, Cache::Strategy::FullCache>*>(&attr);
    return index_root->IsFileName() ? index_root : nullptr;
  } else {
    return nullptr;
  }
}

// FindSubEntry()'s walk down the B+ tree, trusting the entries to be sorted
// by the volume's collation order. A name that sorts before a leaf entry is
// reported absent.
template <Cache::Strategy S>
std::optional<IndexEntry> Io::DirectoryIndex<S>::FindSubEntryInOrder(
    std::wstring_view file_name, const UpCase::Table& upcase) const {
  // Start searching from IndexRoot (B+ tree root node)
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      record_.GetAttr(Attr::Type::IndexRoot);
  if (vec.empty()) {
    return {};
  }

  const std::vector<IndexEntryView>* all_ie =
      FileNameIndexRootEntries(*vec.front());
  if (all_ie == nullptr) {
    return {};
  }

  IndexSearch search{file_name, upcase, {}};
  return SearchIndexEntries(*all_ie, search, /*child_depth=*/0,
                            /*at_root=*/true);
}

// Visit IndexBlocks recursivly to find a specific Filename
template <Cache::Strategy S>
// The depth limit and the visited-VCN set bound the recursion.
// NOLINTNEXTLINE(misc-no-recursion)
std::optional<IndexEntry>
    Io::DirectoryIndex<S>::VisitIndexBlock(ULONGLONG vcn, IndexSearch& search,
                                           size_t depth) const {
  if (depth >= max_index_block_depth) {
    Log::Warn("VisitIndexBlock() aborting: recursion depth limit exceeded");
    return {};
  }

  // A subnode VCN already on this walk means the on-disk B+ tree is
  // malformed (self-loop or cycle) - stop instead of recursing forever.
  if (!search.visited_vcns.insert(vcn).second) {
    return {};
  }

  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      record_.GetAttr(Attr::Type::IndexAllocation);
  if (vec.empty()) {
    return {};
  }

  Attr::IndexBlock index_block;
  if (!static_cast<Attr::AttrIndexAlloc<S>*>(vec.front().get())
           ->ParseIndexBlock(vcn, index_block)) {
    return {};
  }

  // Block entries log their match as a sub-node match, never as a root one.
  return SearchIndexEntries(index_block, search, depth + 1,
                            /*at_root=*/false);
}

// Scans one node's entries in collation order: a match returns a copy, a
// bigger name is skipped, and a smaller one descends into its sub-node.
template <Cache::Strategy S>
// The depth limit and the visited-VCN set bound the recursion.
// NOLINTNEXTLINE(misc-no-recursion)
std::optional<IndexEntry> Io::DirectoryIndex<S>::SearchIndexEntries(
    const std::vector<IndexEntryView>& entries, IndexSearch& search,
    size_t child_depth, bool at_root) const {
  for (const IndexEntryView& index_entry : entries) {
    if (index_entry.HasName()) {
      // Compare name
      const int comparison =
          index_entry.Compare(search.file_name, search.upcase);
      if (comparison == 0) {
        // Must be a copy: the view dies with its node (this FileRecord for
        // the root), the IndexEntry keeps its own bytes.
        LogIndexMatch(at_root);
        return IndexEntry(index_entry);
      }
      // Just step forward if fileName is bigger than IndexEntry
      if (comparison > 0) {
        continue;
      }
      // fileName is smaller than a leaf entry: not found
      if (!index_entry.IsSubNodePtr()) {
        return {};
      }
    }

    // Only a sub-node pointer is worth a visit from here on.
    if (!index_entry.IsSubNodePtr()) {
      continue;
    }
    // Search in SubNode (IndexBlock), recursive call
    std::optional<IndexEntry> retval =
        VisitIndexBlock(index_entry.GetSubNodeVCN(), search, child_depth);
    if (retval) {
      return retval;
    }
  }

  return {};
}

// Traverse SubNode recursivly in ascending order
// Call user defined callback routine once found an subentry
// visitedVcns guards against a malformed/malicious B+ tree where a
// subnode VCN is revisited, which would otherwise recurse without bound.
template <Cache::Strategy S>
// The depth limit and the visited-VCN set bound the recursion.
// NOLINTNEXTLINE(misc-no-recursion)
void Io::DirectoryIndex<S>::TraverseSubNode(
    ULONGLONG vcn, const SubentryCallback& se_call_back, void* context,
    std::unordered_set<ULONGLONG>& visited_vcns, size_t depth) const {
  if (depth >= max_index_block_depth) {
    Log::Warn("TraverseSubNode() aborting: recursion depth limit exceeded");
    return;
  }

  // A subnode VCN already on this walk means the on-disk B+ tree is
  // malformed (self-loop or cycle) - stop instead of recursing forever.
  if (!visited_vcns.insert(vcn).second) {
    return;
  }

  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      record_.GetAttr(Attr::Type::IndexAllocation);
  if (vec.empty()) {
    return;
  }

  Attr::IndexBlock index_block;
  if (!static_cast<Attr::AttrIndexAlloc<S>*>(vec.front().get())
           ->ParseIndexBlock(vcn, index_block)) {
    return;
  }

  for (const IndexEntryView& index_entry : index_block) {
    if (index_entry.IsSubNodePtr()) {
      // recursive call
      TraverseSubNode(index_entry.GetSubNodeVCN(), se_call_back, context,
                      visited_vcns, depth + 1);
    }

    if (index_entry.HasName()) {
      se_call_back(index_entry, context);
    }
  }
}

// Works out how the orphan scan walks alloc: the VCN step per block, and how
// many blocks it visits.
template <Cache::Strategy S>
OrphanScanPlan Io::DirectoryIndex<S>::PlanOrphanScan(
    const Attr::AttrIndexAlloc<S>& alloc) const {
  const DWORD index_block_size = record_.GetVolume().GetIndexBlockSize();
  const DWORD cluster_size = record_.GetVolume().GetClusterSize();
  const ULONGLONG clusters_per_block =
      ClustersPerIndexBlock(index_block_size, cluster_size);

  // GetDataSize() (declared real_size) can be forged far past what the
  // attribute's own data runs actually map; the mapped VCN range, every merged
  // instance included, bounds the scan to what the run list claims to cover
  // instead, so a forged size alone can't drive tens of thousands of doomed
  // ParseIndexBlock() calls. A range of clusters counts in bytes, not in
  // clusters, since a block can be smaller than one.
  const ULONGLONG mapped_clusters =
      (alloc.GetLastVcn() >= alloc.GetStartVcn()) ? alloc.TotalClusters() : 0;
  const ULONGLONG mapped_block_count =
      MappedIndexBlocks(mapped_clusters, cluster_size, index_block_size);

  const ULONGLONG declared_block_count = alloc.GetIndexBlockCount();
  const ULONGLONG block_count = (declared_block_count < mapped_block_count)
                                    ? declared_block_count
                                    : mapped_block_count;
  const ULONGLONG scan_limit = (block_count < max_orphan_scan_blocks)
                                   ? block_count
                                   : max_orphan_scan_blocks;
  // The scan stops short when the mapped range or the cap is below the count.
  const bool capped = declared_block_count > max_orphan_scan_blocks ||
                      declared_block_count > mapped_block_count;
  return {clusters_per_block, declared_block_count, scan_limit, capped};
}

// Reports the entries of one orphaned index block that
// IsOrphanEntryReportable() accepts. A block that fails to parse is skipped.
template <Cache::Strategy S>
void Io::DirectoryIndex<S>::ReportOrphanedBlock(
    Attr::AttrIndexAlloc<S>& alloc, ULONGLONG vcn,
    const SubentryCallback& se_call_back, void* context) const {
  Attr::IndexBlock index_block;
  if (!alloc.ParseIndexBlock(vcn, index_block)) {
    return;
  }

  Log::Info("TraverseSubEntries() recovery: reporting orphaned index block {}",
            vcn);

  const OrphanOwner owner = OwnerOf(record_);
  const bool include_deleted = record_.GetVolume().GetOptions().include_deleted;

  for (const IndexEntryView& index_entry : index_block) {
    if (index_entry.HasName() &&
        IsOrphanEntryReportable(index_entry, owner,
                                include_deleted ? nullptr
                                                : &record_.GetVolume())) {
      se_call_back(index_entry, context);
    }
  }
}

// Recovery pass for TraverseSubEntries(): a corrupt $INDEX_ROOT or internal
// node can leave real $INDEX_ALLOCATION blocks with no surviving pointer to
// them. Since every name appears exactly once in the B+ tree, scanning every
// block the normal walk missed finds them without relying on any pointer at
// all - unlike the normal walk, in VCN order rather than collation order.
template <Cache::Strategy S>
void Io::DirectoryIndex<S>::ScanOrphanedIndexBlocks(
    const SubentryCallback& se_call_back, void* context,
    std::unordered_set<ULONGLONG>& visited_vcns) const {
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      record_.GetAttr(Attr::Type::IndexAllocation);
  if (vec.empty()) {
    return;
  }

  auto* alloc = static_cast<Attr::AttrIndexAlloc<S>*>(vec.front().get());
  const OrphanScanPlan plan = PlanOrphanScan(*alloc);
  if (plan.capped) {
    Log::Info(
        "TraverseSubEntries() recovery: orphan scan capped at {} of {} "
        "index blocks",
        plan.scan_limit, plan.declared_blocks);
  }

  for (ULONGLONG block_index = 0; block_index < plan.scan_limit;
       block_index++) {
    const ULONGLONG vcn = block_index * plan.clusters_per_block;
    if (!visited_vcns.insert(vcn).second) {
      continue;
    }

    ReportOrphanedBlock(*alloc, vcn, se_call_back, context);
  }
}

template class DirectoryIndex<Cache::Strategy::NoCache>;
template class DirectoryIndex<Cache::Strategy::FullCache>;

}  // namespace NtfsBrowser::Io
