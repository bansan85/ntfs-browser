#include "io/orphan-block-scanner.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "attr/base.h"
#include "attr/index-alloc.h"
#include "attr/index-block.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "index-entry.h"
#include "io/entry-prober.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "mft/file-reference.h"
#include "ntfs-volume.h"
#include "record/carved-name.h"
#include "record/slack-carver.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Caps the orphan-block recovery scan: an attacker-controlled declared block
// count must not drive an unbounded number of ParseIndexBlock() calls. 65536
// blocks is already far past any real directory's index, so this only ever
// binds on a forged/damaged $INDEX_ALLOCATION.
constexpr size_t max_orphan_scan_blocks = 65536;

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

// Decides whether a name filed under parent_record at parent_sequence belongs
// to owner's directory. A stale name may be left over from a file already
// deleted from this directory, or from an earlier directory that used this
// record: only one still filed under this very directory counts. Same rule as
// MftTree: a freed directory keeps the entries filed under its sequence from
// before NTFS bumped it.
bool IsFiledUnder(ULONGLONG parent_record, WORD parent_sequence,
                  const OrphanOwner& owner) {
  return !owner.file_ref ||
         (parent_record == *owner.file_ref &&
          Mft::IsSameRecordGeneration(parent_sequence, owner.sequence,
                                      owner.in_use));
}

// Whether findings already holds a candidate that says what candidate says.
bool IsRepeatedCandidate(const SlackFindings& findings,
                         const Record::CarvedName& candidate) {
  const auto [first, last] =
      findings.candidate_by_name.equal_range(candidate.name);
  return std::any_of(first, last, [&](const auto& seen) {
    return Record::SameCarvedName(findings.candidates.at(seen.second),
                                  candidate);
  });
}

}  // namespace

template <Cache::Strategy S>
Io::OrphanBlockScanner<S>::OrphanBlockScanner(
    const FileRecord<S>& record) noexcept
    : record_(record),
      owner_{record.GetFileReference(), record.GetSequenceNumber(),
             !record.IsDeleted()} {}

template <Cache::Strategy S>
bool Io::OrphanBlockScanner<S>::KnowsOwner() const noexcept {
  return owner_.file_ref.has_value();
}

// Works out how the orphan scan walks alloc: the VCN step per block, and how
// many blocks it visits.
template <Cache::Strategy S>
OrphanScanPlan Io::OrphanBlockScanner<S>::PlanOrphanScan(
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

// Decides whether an entry found in an orphaned block is authentic. It MUST be
// filed under the directory. Visibility of the named record is left to the
// walk that delivers it.
template <Cache::Strategy S>
bool Io::OrphanBlockScanner<S>::IsReportable(
    const IndexEntryView& entry) const {
  return IsFiledUnder(entry.GetParentReference(),
                      entry.GetParentSequenceNumber(), owner_);
}

// Probes together the records that the entries of one orphaned block name,
// when the walk is batched. Only the entries that IsReportable() accepts: the
// others are never delivered.
template <Cache::Strategy S>
void Io::OrphanBlockScanner<S>::ProbeReportable(
    const std::vector<IndexEntryView>& entries, EntryProber<S>* prober) const {
  if (prober == nullptr) {
    return;
  }
  std::vector<ULONGLONG> records;
  records.reserve(entries.size());
  for (const IndexEntryView& entry : entries) {
    if (entry.HasName() && IsReportable(entry)) {
      records.push_back(entry.GetFileReference());
    }
  }
  prober->ProbeGroup(records);
}

// Reports the entries of one orphaned index block that IsReportable()
// accepts. A block that fails to parse is skipped.
template <Cache::Strategy S>
void Io::OrphanBlockScanner<S>::ReportOrphanedBlock(
    Attr::AttrIndexAlloc<S>& alloc, ULONGLONG vcn, EntryProber<S>* prober,
    const DeliverCallback& deliver) const {
  Attr::IndexBlock index_block;
  if (!alloc.ParseIndexBlock(vcn, index_block)) {
    return;
  }

  Log::Info("TraverseSubEntries() recovery: reporting orphaned index block {}",
            vcn);

  ProbeReportable(index_block, prober);
  for (const IndexEntryView& index_entry : index_block) {
    if (index_entry.HasName() && IsReportable(index_entry)) {
      deliver(index_entry);
    }
  }
}

template <Cache::Strategy S>
void Io::OrphanBlockScanner<S>::Scan(
    EntryProber<S>* prober, std::unordered_set<ULONGLONG>& visited_vcns,
    const DeliverCallback& deliver) const {
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

    ReportOrphanedBlock(*alloc, vcn, prober, deliver);
  }
}

// Keeps the candidates of one block that name the directory and that no
// earlier candidate repeats. Stops at the cap.
template <Cache::Strategy S>
void Io::OrphanBlockScanner<S>::KeepSlackCandidates(
    std::vector<Record::CarvedName> carved, SlackFindings& findings) const {
  for (Record::CarvedName& candidate : carved) {
    // The parent is the only proof a candidate belongs to this directory.
    if (!candidate.parent_known ||
        !IsFiledUnder(candidate.parent_record, candidate.parent_sequence,
                      owner_) ||
        IsRepeatedCandidate(findings, candidate)) {
      continue;
    }
    if (findings.candidates.size() >= max_index_slack_candidates) {
      findings.capped = true;
      return;
    }
    findings.candidate_by_name.emplace(candidate.name,
                                       findings.candidates.size());
    findings.candidates.push_back(std::move(candidate));
  }
}

// Adds the live names of one block to findings, then its slack candidates. A
// block that fails its checks, or whose entries are damaged, is skipped.
template <Cache::Strategy S>
void Io::OrphanBlockScanner<S>::CarveBlockSlack(Attr::AttrIndexAlloc<S>& alloc,
                                                ULONGLONG vcn,
                                                SlackFindings& findings) const {
  Attr::IndexBlock index_block;
  if (!alloc.ParseIndexBlock(vcn, index_block)) {
    return;
  }
  const std::optional<size_t> used_end = index_block.UsedEnd();
  if (!used_end) {
    Log::Info("CarveSlackNames() recovery: index block {} is damaged", vcn);
    return;
  }
  for (const IndexEntryView& entry : index_block) {
    if (entry.HasName()) {
      findings.live_names.emplace(entry.GetFilename());
    }
  }
  KeepSlackCandidates(
      Record::CarveFileNamesFrom(index_block.Bytes(), *used_end,
                                 Record::CarveSource::IndexSlack),
      findings);
}

template <Cache::Strategy S>
void Io::OrphanBlockScanner<S>::CarveSlack(Attr::AttrIndexAlloc<S>& alloc,
                                           SlackFindings& findings) const {
  const OrphanScanPlan plan = PlanOrphanScan(alloc);
  for (ULONGLONG block_index = 0;
       block_index < plan.scan_limit && !findings.capped; block_index++) {
    CarveBlockSlack(alloc, block_index * plan.clusters_per_block, findings);
  }
}

template class OrphanBlockScanner<Cache::Strategy::NoCache>;
template class OrphanBlockScanner<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
