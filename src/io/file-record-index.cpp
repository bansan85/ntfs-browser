#include "io/file-record-index.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "attr/base.h"
#include "attr/index-alloc.h"
#include "attr/index-block.h"
#include "attr/index-root.h"
#include "attr/resident.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "index-entry.h"
#include "io/entry-prober.h"
#include "io/file-record-attr-types.h"
#include "io/file-record.h"
#include "io/orphan-block-scanner.h"
#include "io/record-standing.h"
#include "log/ntfs-common.h"
#include "ntfs-volume.h"
#include "record/carved-name.h"
#include "upcase/upcase.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

template <Cache::Strategy S>
class AttrBase;

}  // namespace Attr
template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Chosen well above any real NTFS directory's B+ tree depth, but low enough
// to unwind long before a forged chain overflows the stack.
constexpr size_t max_index_block_depth = 64;

// Logs a name match found by SearchIndexEntries(). The root and the index
// blocks log under different names.
void LogIndexMatch(bool at_root) {
  if (at_root) {
    Log::Debug("FindSubEntry() found entry in Index Root");
  } else {
    Log::Debug("VisitIndexBlock() found entry in sub-node");
  }
}

// The records that the entries of one index node name: what ProbeNode() hands
// to the prober. Empty when no entry names one.
std::vector<ULONGLONG>
    NamedRecordsOf(const std::vector<IndexEntryView>& entries) {
  std::vector<ULONGLONG> records;
  records.reserve(entries.size());
  for (const IndexEntryView& entry : entries) {
    if (entry.HasName()) {
      records.push_back(entry.GetFileReference());
    }
  }
  return records;
}

// Probes together the records that the entries of one index node name, when
// the walk is batched.
template <Cache::Strategy S>
void ProbeNode(const std::vector<IndexEntryView>& entries,
               WalkTarget<S>& target) {
  if (target.prober == nullptr) {
    return;
  }
  target.prober->ProbeGroup(NamedRecordsOf(entries));
}

// Decides whether a normal walk or name lookup skips entry, given what the
// probe of the record it names found. A named record that does not parse is
// damaged, not freed: it is skipped unless recover_errors is on, whatever
// include_deleted says. With include_deleted off, an entry whose named record
// is freed, or reused under another sequence number, is hidden, unless the
// directory is volume metadata (bypass_deleted_gate): its entries name metadata
// files, which are read whatever include_deleted says.
template <Cache::Strategy S>
bool IsHiddenGiven(const NamedState& named, const IndexEntryView& entry,
                   const NtfsVolume<S>& volume, bool bypass_deleted_gate) {
  const bool recover = volume.GetOptions().recover_errors;
  if (!named.parsed) {
    Log::Recoverable(recover, "Entry names record {} that does not parse",
                     entry.GetFileReference());
    return !recover;
  }
  if (volume.GetOptions().include_deleted || bypass_deleted_gate) {
    return false;
  }
  return named.deleted || named.sequence != entry.GetSequenceNumber();
}

// Probes the record entry names, then decides as IsHiddenGiven() does.
template <Cache::Strategy S>
bool IsEntryHidden(const IndexEntryView& entry, const NtfsVolume<S>& volume,
                   bool bypass_deleted_gate) {
  Io::FileRecord<S> named(volume);
  NamedState state;
  state.parsed = named.ParseFileRecord(entry.GetFileReference());
  if (state.parsed) {
    state.deleted = named.IsDeleted();
    state.sequence = named.GetSequenceNumber();
  }
  return IsHiddenGiven(state, entry, volume, bypass_deleted_gate);
}

// How the record that entry names stands, as the batched walk reports it to its
// callback. A reference that carries sequence 0 claims no generation.
RecordStanding StandingOf(const NamedState& named,
                          const IndexEntryView& entry) {
  if (!named.parsed) {
    return RecordStanding::Unreadable;
  }
  if (named.deleted) {
    return RecordStanding::Deleted;
  }
  const bool reused = entry.GetSequenceNumber() != 0 &&
                      entry.GetSequenceNumber() != named.sequence;
  return reused ? RecordStanding::Reused : RecordStanding::Live;
}

// What a scan of one node does with an entry when it looks for a name.
enum class SearchStep : std::uint8_t {
  // The entry equals the name.
  Equal,
  // Neither the entry nor anything below it can be the name.
  Skip,
  // The name sorts before a leaf entry: it is not in the index.
  NotFound,
  // The name sorts before the entry, or the entry has no name: its sub-node is
  // worth a visit.
  Descend
};

// Compares the entry with the name searched. The index is in collation order,
// so the result tells whether the name can still be at or below the entry.
SearchStep StepFor(const IndexEntryView& entry, const IndexSearch& search) {
  if (entry.HasName()) {
    const int comparison = entry.Compare(search.file_name, search.upcase);
    if (comparison == 0) {
      return SearchStep::Equal;
    }
    if (comparison > 0) {
      return SearchStep::Skip;
    }
    if (!entry.IsSubNodePtr()) {
      return SearchStep::NotFound;
    }
  }
  return entry.IsSubNodePtr() ? SearchStep::Descend : SearchStep::Skip;
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

  WalkTarget<S> target;
  target.plain = &se_call_back;
  target.context = context;
  Walk(target);
}

template <Cache::Strategy S>
void Io::DirectoryIndex<S>::TraverseWithStanding(
    const StandingCallback& se_call_back, void* context) const {
  assert(se_call_back);

  EntryProber<S> prober(record_.GetVolume(), &record_.GetProbeStore());
  WalkTarget<S> target;
  target.standing = &se_call_back;
  target.context = context;
  target.prober = &prober;
  Walk(target);
}

// Sends entry to the walk's callback, unless the record it names says to hide
// it. The default walk probes the record now. The batched walk takes the
// outcome of the probe its node made.
template <Cache::Strategy S>
void Io::DirectoryIndex<S>::Deliver(const IndexEntryView& entry,
                                    WalkTarget<S>& target) const {
  const NtfsVolume<S>& volume = record_.GetVolume();
  if (target.prober == nullptr) {
    if (!IsEntryHidden(entry, volume, record_.BypassesDeletedGate())) {
      (*target.plain)(entry, target.context);
    }
    return;
  }

  const NamedState named = target.prober->Take(entry.GetFileReference());
  if (!IsHiddenGiven(named, entry, volume, record_.BypassesDeletedGate())) {
    (*target.standing)(entry, StandingOf(named, entry), target.context);
  }
}

// The walk of Traverse() and TraverseWithStanding(), from the $INDEX_ROOT.
template <Cache::Strategy S>
void Io::DirectoryIndex<S>::Walk(WalkTarget<S>& target) const {
  const NtfsVolume<S>& volume = record_.GetVolume();
  const bool recover = volume.GetOptions().recover_errors;

  // Start traversing from IndexRoot (B+ tree root node)

  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      record_.GetAttr(Attr::Type::IndexRoot);
  if (vec.empty()) {
    // No IndexRoot at all to start the normal walk from, but $INDEX_ALLOCATION
    // blocks may still exist and hold every entry.
    if (recover) {
      std::unordered_set<ULONGLONG> visited_vcns;
      ScanOrphanedIndexBlocks(target, visited_vcns);
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

  ProbeNode(all_ie, target);
  for (const IndexEntryView& index_entry : all_ie) {
    // Visit subnode first
    if (index_entry.IsSubNodePtr()) {
      TraverseSubNode(index_entry.GetSubNodeVCN(), target, visited_vcns, 0);
    }

    if (index_entry.HasName()) {
      Deliver(index_entry, target);
    }
  }

  if (recover) {
    ScanOrphanedIndexBlocks(target, visited_vcns);
  }
}

template <Cache::Strategy S>
std::optional<typename Io::DirectoryIndex<S>::RootChildren>
    Io::DirectoryIndex<S>::RootChildBlocks() const {
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& roots =
      record_.GetAttr(Attr::Type::IndexRoot);
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& allocs =
      record_.GetAttr(Attr::Type::IndexAllocation);
  if (roots.empty() || allocs.empty()) {
    return std::nullopt;
  }
  const std::vector<IndexEntryView>* entries =
      FileNameIndexRootEntries(*roots.front());
  if (entries == nullptr) {
    return std::nullopt;
  }

  RootChildren children{
      .alloc = static_cast<Attr::AttrIndexAlloc<S>*>(allocs.front().get()),
      .vcns = {}};
  // The walk visits a VCN once, the first time it meets it.
  std::unordered_set<ULONGLONG> seen;
  for (const IndexEntryView& entry : *entries) {
    if (entry.IsSubNodePtr() && seen.insert(entry.GetSubNodeVCN()).second) {
      children.vcns.push_back(entry.GetSubNodeVCN());
    }
  }
  if (children.vcns.empty()) {
    return std::nullopt;
  }
  return children;
}

// Find a specific Filename from the B+ tree, then, if the name is missing and
// the volume has no $UpCase table, from every entry. An entry spelled exactly
// as file_name wins over one that differs by case.
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
        if (index_entry.Compare(file_name, upcase) != 0) {
          return;
        }
        // Only the first entry of each kind counts: an exact one replaces
        // an equal one, never the other way round.
        const bool exact = index_entry.GetFilename() == file_name;
        if (!found || (exact && found->GetFilename() != file_name)) {
          found.emplace(index_entry);
        }
      },
      nullptr);
  return found;
}

// The entries of a file-name $INDEX_ROOT, or null for any other kind of index.
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
    static_assert(S == Cache::Strategy::FullCache,
                  "FileNameIndexRootEntries() has no arm for this strategy");
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

  IndexSearch search{
      .file_name = file_name, .upcase = upcase, .visited_vcns = {}};
  std::optional<IndexEntry> exact =
      SearchIndexEntries(*all_ie, search, /*child_depth=*/0,
                         /*at_root=*/true);
  if (exact || !search.fallback) {
    return exact;
  }
  // No entry is spelled like file_name: the first one equal to it stands in.
  LogIndexMatch(search.fallback_at_root);
  return std::move(search.fallback);
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

// An entry of the node that is equal to the name. Returns a copy of it when it
// is spelled exactly like the name, or the twin found below it. Otherwise
// keeps it as the fallback, unless it is hidden, and returns nothing.
template <Cache::Strategy S>
// The depth limit and the visited-VCN set bound the recursion.
// NOLINTNEXTLINE(misc-no-recursion)
std::optional<IndexEntry> Io::DirectoryIndex<S>::MatchEqualEntry(
    const IndexEntryView& index_entry, IndexSearch& search, size_t child_depth,
    bool at_root) const {
  // A hidden entry is as good as absent: an entry of another spelling can
  // still stand for it.
  const bool hidden = IsEntryHidden(index_entry, record_.GetVolume(),
                                    record_.BypassesDeletedGate());
  search.met_equal = true;
  if (!hidden && index_entry.GetFilename() == search.file_name) {
    // Must be a copy: the view dies with its node (this FileRecord for the
    // root), the IndexEntry keeps its own bytes.
    LogIndexMatch(at_root);
    return IndexEntry(index_entry);
  }
  // The entries below this one come first in index order, and a twin spelled
  // like the name can sit there.
  if (index_entry.IsSubNodePtr()) {
    std::optional<IndexEntry> twin =
        VisitIndexBlock(index_entry.GetSubNodeVCN(), search, child_depth);
    if (twin) {
      return twin;
    }
  }
  if (!hidden && !search.fallback) {
    search.fallback.emplace(index_entry);
    search.fallback_at_root = at_root;
  }
  return {};
}

// Scans one node's entries in collation order: an entry spelled like the name
// returns a copy, an equal one that is spelled otherwise is kept as the
// fallback and the scan goes on, a bigger name is skipped, and a smaller one
// descends into its sub-node.
template <Cache::Strategy S>
// The depth limit and the visited-VCN set bound the recursion.
// NOLINTNEXTLINE(misc-no-recursion)
std::optional<IndexEntry> Io::DirectoryIndex<S>::SearchIndexEntries(
    const std::vector<IndexEntryView>& entries, IndexSearch& search,
    size_t child_depth, bool at_root) const {
  for (const IndexEntryView& index_entry : entries) {
    switch (StepFor(index_entry, search)) {
      case SearchStep::Equal:
        if (std::optional<IndexEntry> found =
                MatchEqualEntry(index_entry, search, child_depth, at_root)) {
          return found;
        }
        break;

      case SearchStep::Skip:
        break;

      case SearchStep::NotFound:
        return {};

      case SearchStep::Descend:
        // Search in SubNode (IndexBlock), recursive call
        if (std::optional<IndexEntry> retval = VisitIndexBlock(
                index_entry.GetSubNodeVCN(), search, child_depth)) {
          return retval;
        }
        // Every later entry is greater than this one: once an equal entry was
        // met, kept or hidden, none of their sub-nodes can hold a twin of it. A
        // search that met nothing equal keeps reading, as it always did.
        if (search.met_equal) {
          return {};
        }
        break;
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
    ULONGLONG vcn, WalkTarget<S>& target,
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

  ProbeNode(index_block, target);
  for (const IndexEntryView& index_entry : index_block) {
    if (index_entry.IsSubNodePtr()) {
      // recursive call
      TraverseSubNode(index_entry.GetSubNodeVCN(), target, visited_vcns,
                      depth + 1);
    }

    if (index_entry.HasName()) {
      Deliver(index_entry, target);
    }
  }
}

// Walks the held part of the B+ tree under the block at vcn as
// TraverseSubNode() does, and adds the records each node names to groups, a
// node before its children. A block the source does not hold, or that does not
// parse, stops the descent.
template <Cache::Strategy S>
// The depth limit and the visited-VCN set bound the recursion.
// NOLINTNEXTLINE(misc-no-recursion)
void Io::DirectoryIndex<S>::CollectProbeGroups(
    ULONGLONG vcn, std::unordered_set<ULONGLONG>& visited_vcns, size_t depth,
    std::vector<std::vector<ULONGLONG>>& groups) const {
  if (depth >= max_index_block_depth || !visited_vcns.insert(vcn).second) {
    return;
  }

  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& vec =
      record_.GetAttr(Attr::Type::IndexAllocation);
  Attr::IndexBlock index_block;
  if (vec.empty() || !static_cast<Attr::AttrIndexAlloc<S>*>(vec.front().get())
                          ->ParseHeldIndexBlock(vcn, index_block)) {
    return;
  }

  std::vector<ULONGLONG> records = NamedRecordsOf(index_block);
  if (!records.empty()) {
    groups.push_back(std::move(records));
  }
  for (const IndexEntryView& index_entry : index_block) {
    if (index_entry.IsSubNodePtr()) {
      CollectProbeGroups(index_entry.GetSubNodeVCN(), visited_vcns, depth + 1,
                         groups);
    }
  }
}

template <Cache::Strategy S>
std::vector<std::vector<ULONGLONG>>
    Io::DirectoryIndex<S>::HeldProbeGroups() const {
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& roots =
      record_.GetAttr(Attr::Type::IndexRoot);
  if (roots.empty()) {
    return {};
  }
  const std::vector<IndexEntryView>* root_entries =
      FileNameIndexRootEntries(*roots.front());
  if (root_entries == nullptr) {
    return {};
  }

  // The parse of a held block logs when the block is damaged, and counts what
  // it salvages: the walk does both when it reaches the block.
  Log::Detail::CapturedLog discarded;
  discarded.Reset(0);
  const Log::Detail::CapturedLog::Mark before = discarded.End();
  std::vector<std::vector<ULONGLONG>> groups;
  {
    const Log::Detail::CaptureScope scope(discarded);
    std::vector<ULONGLONG> records = NamedRecordsOf(*root_entries);
    if (!records.empty()) {
      groups.push_back(std::move(records));
    }
    std::unordered_set<ULONGLONG> visited_vcns;
    for (const IndexEntryView& index_entry : *root_entries) {
      if (index_entry.IsSubNodePtr()) {
        CollectProbeGroups(index_entry.GetSubNodeVCN(), visited_vcns, 0,
                           groups);
      }
    }
  }
  Log::Detail::ForgetRecoveredSince(before);
  return groups;
}

// Recovery pass of Walk(): hands the entries of the blocks that the walk
// missed to Deliver().
template <Cache::Strategy S>
void Io::DirectoryIndex<S>::ScanOrphanedIndexBlocks(
    WalkTarget<S>& target, std::unordered_set<ULONGLONG>& visited_vcns) const {
  const OrphanBlockScanner<S> scanner(record_);
  scanner.Scan(target.prober, visited_vcns,
               [&](const IndexEntryView& entry) { Deliver(entry, target); });
}

// Looks for the names of deleted files in the unused space of every valid
// $INDEX_ALLOCATION block. A deleted entry is not erased: NTFS closes the gap
// by moving the entries after it, and the tail of the block keeps the old
// bytes. The root has no such space, since its value ends where its entries
// do.
template <Cache::Strategy S>
std::vector<Record::CarvedName> Io::DirectoryIndex<S>::CarveSlackNames() const {
  const VolumeOptions& options = record_.GetVolume().GetOptions();
  const OrphanBlockScanner<S> scanner(record_);
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& alloc_attrs =
      record_.GetAttr(Attr::Type::IndexAllocation);
  if (!options.recover_errors || !options.include_deleted ||
      !scanner.KnowsOwner() || alloc_attrs.empty()) {
    return {};
  }

  SlackFindings findings;
  const std::vector<std::unique_ptr<Attr::AttrBase<S>>>& roots =
      record_.GetAttr(Attr::Type::IndexRoot);
  if (!roots.empty()) {
    // An index of anything but file names holds no names to carve.
    const std::vector<IndexEntryView>* root_entries =
        FileNameIndexRootEntries(*roots.front());
    if (root_entries == nullptr) {
      return {};
    }
    for (const IndexEntryView& entry : *root_entries) {
      if (entry.HasName()) {
        findings.live_names.emplace(entry.GetFilename());
      }
    }
  }

  auto* alloc =
      static_cast<Attr::AttrIndexAlloc<S>*>(alloc_attrs.front().get());
  scanner.CarveSlack(*alloc, findings);

  std::erase_if(findings.candidates, [&](const Record::CarvedName& candidate) {
    return findings.live_names.contains(candidate.name);
  });
  if (findings.capped) {
    Log::Info("CarveSlackNames() recovery: stopped at {} candidates",
              max_index_slack_candidates);
  }
  return std::move(findings.candidates);
}

template class DirectoryIndex<Cache::Strategy::NoCache>;
template class DirectoryIndex<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
