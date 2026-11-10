#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "index-entry.h"
#include "internal-export.h"
#include "io/file-record.h"
#include "record/carved-name.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

namespace Attr {

template <Cache::Strategy S>
class AttrIndexAlloc;

}  // namespace Attr

namespace Io {

template <Cache::Strategy S>
class EntryProber;

// Caps the candidates the slack pass keeps: a forged directory of 65536 blocks
// could hold millions of look-alikes. About 100 MiB of names at most, and far
// above what the unused space of a real directory yields.
inline constexpr size_t max_index_slack_candidates = size_t{1} << 20;

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

// The directory an orphan scan reports entries for: its file reference when
// known, and the sequence number and liveness it has now.
struct OrphanOwner {
  std::optional<ULONGLONG> file_ref;
  WORD sequence;
  bool in_use;
};

// What the slack pass has found so far in one directory.
struct SlackFindings {
  // Names of the entries in the used part of the root and of every block.
  std::unordered_set<std::wstring> live_names;
  // Candidates, each reported once. The live names are removed at the end.
  std::vector<Record::CarvedName> candidates;
  // Where each candidate sits in candidates, by name: finds a repeat without
  // comparing with every candidate.
  std::unordered_multimap<std::wstring, size_t> candidate_by_name;
  // True once a candidate was refused for lack of room.
  bool capped{false};
};

// The recovery passes over the $INDEX_ALLOCATION blocks of one directory that
// the normal walk did not reach: the report of their entries, and the carve of
// the names their unused space still holds. Short-lived: built over a
// FileRecord for one call, never stored.
template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT_TESTS_ONLY OrphanBlockScanner {
 public:
  // Takes each entry that the scan accepts, the way the walk that owns the
  // scan delivers it.
  using DeliverCallback = std::function<void(const IndexEntryView&)>;

  // record MUST outlive this object.
  explicit OrphanBlockScanner(const FileRecord<S>& record) noexcept;

  // True when the file reference of the directory is known. Without it no
  // entry can be proven to belong to the directory.
  [[nodiscard]] bool KnowsOwner() const noexcept;

  // Recovery pass for the walk of a directory: a corrupt $INDEX_ROOT or
  // internal node can leave real $INDEX_ALLOCATION blocks with no surviving
  // pointer to them. Every name appears exactly once in the B+ tree, so a scan
  // of every block the normal walk missed finds them without relying on any
  // pointer at all. It goes in VCN order, not in collation order. A VCN that
  // is in visited_vcns is skipped, and each VCN the scan visits is added to
  // it. prober, when not null, probes the records that the entries of each
  // block name before deliver gets them.
  void Scan(EntryProber<S>* prober, std::unordered_set<ULONGLONG>& visited_vcns,
            const DeliverCallback& deliver) const;

  // Adds the live names and the slack candidates of every block of alloc to
  // findings, up to max_index_slack_candidates.
  void CarveSlack(Attr::AttrIndexAlloc<S>& alloc,
                  SlackFindings& findings) const;

 private:
  // The record whose index this scans. It MUST outlive this object.
  const FileRecord<S>& record_;
  // Who the entries must be filed under.
  OrphanOwner owner_;

  [[nodiscard]] OrphanScanPlan
      PlanOrphanScan(const Attr::AttrIndexAlloc<S>& alloc) const;
  [[nodiscard]] bool IsReportable(const IndexEntryView& entry) const;
  void ProbeReportable(const std::vector<IndexEntryView>& entries,
                       EntryProber<S>* prober) const;
  void ReportOrphanedBlock(Attr::AttrIndexAlloc<S>& alloc, ULONGLONG vcn,
                           EntryProber<S>* prober,
                           const DeliverCallback& deliver) const;
  void CarveBlockSlack(Attr::AttrIndexAlloc<S>& alloc, ULONGLONG vcn,
                       SlackFindings& findings) const;
  void KeepSlackCandidates(std::vector<Record::CarvedName> carved,
                           SlackFindings& findings) const;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
