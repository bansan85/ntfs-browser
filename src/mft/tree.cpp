#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/usn/types.h>

#include "alloc/claim-inventory.h"
#include "log/ntfs-common.h"
#include "mft/cluster-accounts.h"
#include "mft/entry-links.h"
#include "mft/entry-table.h"
#include "mft/journal-annotator.h"
#include "mft/path-builder.h"
#include "mft/slot-scan.h"
#include "mft/used-slots.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// Entries a scan reserves room for at most, from the count the $MFT bitmap
// gives. 2^20 covers most volumes, and bounds what a forged bitmap makes a
// scan allocate before it reads a record: about 200 MiB.
constexpr ULONGLONG max_reserved_entries = ULONGLONG{1} << 20;

// The optional work a scan of volume does for each entry.
template <Cache::Strategy S>
SlotFeatures FeaturesOf(const NtfsVolume<S>& volume,
                        const Mft::MftTree::ScanOptions& options) {
  // Deleted entries only exist with include_deleted, and the annotation reads
  // the bitmap: without both, the scan reads what it always did.
  return {.annotate = volume.GetOptions().include_deleted &&
                      options.annotate_data_state,
          .account = EffectiveAccountLevel(volume, options) !=
                     Alloc::ClusterAccountLevel::Off};
}

// Has the volume keep the windows of $Bitmap that the data states read, for the
// cluster sweep that reads them again, and gives them back when it goes.
template <Cache::Strategy S>
class KeptBitmapWindows {
 public:
  // Keeps nothing unless the scan both annotates and accounts.
  KeptBitmapWindows(const NtfsVolume<S>& volume, SlotFeatures features)
      : volume_(features.account && features.annotate ? &volume : nullptr) {
    if (volume_ != nullptr) {
      volume_->KeepBitmapWindows(
          volume.GetOptions().tuning.mft.max_kept_bitmap_bytes);
    }
  }

  KeptBitmapWindows(const KeptBitmapWindows&) = delete;
  KeptBitmapWindows& operator=(const KeptBitmapWindows&) = delete;
  KeptBitmapWindows(KeptBitmapWindows&&) = delete;
  KeptBitmapWindows& operator=(KeptBitmapWindows&&) = delete;

  ~KeptBitmapWindows() {
    if (volume_ != nullptr) {
      volume_->KeepBitmapWindows(0);
    }
  }

 private:
  const NtfsVolume<S>* volume_;
};

}  // namespace

// The state of a tree: its entries and how they link. Each piece is a class of
// its own: the table of entries, their links, the paths, the cluster accounts
// and the journal.
class Mft::MftTree::Impl {
 public:
  EntryTable table;
  EntryLinks links;
  PathBuilder paths;
  ClusterAccounts accounts;
  Mft::MftTree::ScanStats stats;
  // The files only the journal names, ordered by record, then sequence.
  std::vector<Mft::MftTree::JournalOnlyEntry> journal_only;

  // Reads the record slots of volume's $MFT, keeps each base record's entry,
  // then links the entries into a tree. With include_deleted off, the slots
  // $MFT's $BITMAP marks free are not read.
  template <Cache::Strategy S>
  void Scan(const NtfsVolume<S>& volume,
            const Mft::MftTree::ScanOptions& options);

 private:
  // Reads the slots into the table, indexes them and accounts for clusters.
  // Returns the clusters of $J when the scan met the $UsnJrnl record, or
  // nothing when a run told to stop after a phase ended the scan.
  template <Cache::Strategy S>
  std::optional<std::vector<Alloc::Extent>>
      ReadEntries(const NtfsVolume<S>& volume, const SlotScanInput& input);
  // Annotates the linked entries from the volume's change journal, when
  // recovering and when the scan ran to its end. announced says whether the
  // scan announced the journal to the reader.
  template <Cache::Strategy S>
  void ApplyJournalIfComplete(const NtfsVolume<S>& volume, bool announced);
  // Logs the counters of the scan. The data state counters only mean something
  // when the scan annotated the entries.
  void LogScanSummary(bool annotated) const;
  void LogJournalSummary() const;
};

Mft::MftTree::MftTree(const NtfsVolume<Cache::Strategy::NoCache>& volume,
                      const Mft::MftTree::ScanOptions& options)
    : impl_(std::make_unique<Impl>()) {
  impl_->Scan(volume, options);
}

Mft::MftTree::MftTree(const NtfsVolume<Cache::Strategy::FullCache>& volume,
                      const Mft::MftTree::ScanOptions& options)
    : impl_(std::make_unique<Impl>()) {
  impl_->Scan(volume, options);
}

Mft::MftTree::MftTree(const Mft::MftTree& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

Mft::MftTree::MftTree(Mft::MftTree&& other) noexcept = default;

Mft::MftTree& Mft::MftTree::operator=(const Mft::MftTree& other) {
  if (this != &other) {
    impl_ = std::make_unique<Impl>(*other.impl_);
  }
  return *this;
}

Mft::MftTree& Mft::MftTree::operator=(Mft::MftTree&& other) noexcept = default;

Mft::MftTree::~MftTree() = default;

template <Cache::Strategy S>
void Mft::MftTree::Impl::Scan(const NtfsVolume<S>& volume,
                              const Mft::MftTree::ScanOptions& options) {
  volume.SetReadPattern(Tuning::Pattern::LargeSequential);
  const ULONGLONG total = volume.GetRecordsCount();
  stats.slots = total;

  // A freed record is dropped anyway without include_deleted, so its read is
  // saved. With it, every slot is read, bitmap or not.
  const std::optional<std::vector<BYTE>> bitmap =
      volume.GetOptions().include_deleted ? std::nullopt
                                          : ReadMftBitmap(volume, total);
  // An Entry is big: each time the vector grows, it moves them all. Without a
  // bitmap, the slots are all there is to go by.
  table.Entries().reserve(gsl::narrow<size_t>(
      (std::min)(bitmap ? CountUsedSlots(*bitmap, total) : total,
                 max_reserved_entries)));

  const SlotFeatures features = FeaturesOf(volume, options);
  const std::optional<std::vector<Alloc::Extent>> journal_extents =
      ReadEntries(volume, {options, bitmap, features});
  if (!journal_extents) {
    return;
  }

  if (options.progress && stats.complete) {
    options.progress(total, total);
  }

  // The scan's own hints are withdrawn by now. The reader reads the journal
  // while the tree is linked, and the loader finds its bytes ready.
  const bool journal_announced =
      stats.complete && AnnounceJournal(volume, *journal_extents);
  LogScanSummary(features.annotate);
  {
    const Bench::PhaseScope phase(Bench::Phase::Link);
    stats.unreachable = links.Link(table);
  }
  if (Bench::StopRequested()) {
    stats.complete = false;
    return;
  }
  ApplyJournalIfComplete(volume, journal_announced);
}

template <Cache::Strategy S>
std::optional<std::vector<Alloc::Extent>>
    Mft::MftTree::Impl::ReadEntries(const NtfsVolume<S>& volume,
                                    const SlotScanInput& input) {
  // The data states and the sweep both read $Bitmap: the windows the first
  // reads stay for the second.
  const KeptBitmapWindows<S> kept_windows(volume, input.features);
  Alloc::ClaimInventory inventory;
  std::vector<Alloc::Extent> journal_extents =
      ScanSlots(volume, input, table.Entries(), stats, inventory);
  // A run told to stop after a phase leaves a tree that is not complete.
  if (Bench::StopRequested()) {
    stats.complete = false;
    return std::nullopt;
  }
  table.IndexAll();
  if (input.features.account && stats.complete) {
    const Bench::PhaseScope phase(Bench::Phase::ClusterSweep);
    volume.SetReadPattern(Tuning::Pattern::LargeSequential);
    accounts.Account(volume, input.options, inventory, table);
    stats.cluster_accounts = accounts.Count();
    stats.clusters = accounts.Tally();
  }
  if (Bench::StopRequested()) {
    stats.complete = false;
    return std::nullopt;
  }
  return journal_extents;
}

template <Cache::Strategy S>
void Mft::MftTree::Impl::ApplyJournalIfComplete(const NtfsVolume<S>& volume,
                                                bool announced) {
  // A scan that progress() stopped holds only some records: a file would look
  // lost to the journal that is merely not read yet.
  if (!volume.GetOptions().recover_errors || !stats.complete) {
    return;
  }
  const Bench::PhaseScope phase(Bench::Phase::JournalLoad);
  volume.SetReadPattern(Tuning::Pattern::LargeSequential);
  const Usn::Journal* journal = volume.GetUsnJournal();
  // What the loader did not read of the announced journal is read for nothing.
  if (announced) {
    volume.CancelPrefetch();
  }
  if (journal == nullptr) {
    return;
  }
  journal_only = JournalAnnotator(table, volume.GetOptions().include_deleted)
                     .Apply(*journal, stats);
  LogJournalSummary();
}

void Mft::MftTree::Impl::LogScanSummary(bool annotated) const {
  Log::Info(
      "MFT scan: {} slots, {} in use, {} deleted, {} unreadable, {} skipped",
      stats.slots, stats.in_use, stats.deleted, stats.unreadable,
      stats.skipped);
  if (annotated) {
    Log::Info("MFT scan data: {} intact, {} reallocated, {} unknown, {} bad",
              stats.data_intact, stats.data_reallocated, stats.data_unknown,
              stats.data_bad);
  }
  if (stats.carved_names != 0) {
    Log::Info("MFT scan slack: {} names carved", stats.carved_names);
  }
}

void Mft::MftTree::Impl::LogJournalSummary() const {
  Log::Info(
      "MFT scan journal: {} records, {} entries annotated, {} only in the "
      "journal",
      stats.usn_records, stats.usn_annotated, stats.usn_journal_only);
}

const std::vector<Mft::MftTree::Entry>& Mft::MftTree::Entries() const noexcept {
  return impl_->table.Entries();
}

const Mft::MftTree::Entry* Mft::MftTree::Find(ULONGLONG record) const {
  return impl_->table.Find(record);
}

std::span<const ULONGLONG> Mft::MftTree::Children(ULONGLONG dir_record) const {
  return impl_->links.Children(impl_->table, dir_record);
}

bool Mft::MftTree::IsReachable(ULONGLONG record) const {
  const std::optional<size_t> index = impl_->table.IndexOf(record);
  return index && impl_->links.IsReachable(*index);
}

std::span<const Mft::MftTree::JournalOnlyEntry>
    Mft::MftTree::JournalOnly() const noexcept {
  return impl_->journal_only;
}

const Alloc::ClusterAccount*
    Mft::MftTree::ClusterAccountOf(ULONGLONG record) const {
  return impl_->accounts.Find(record);
}

const Mft::MftTree::ScanStats& Mft::MftTree::Stats() const noexcept {
  return impl_->stats;
}

std::wstring
    Mft::MftTree::GetPath(ULONGLONG record,
                          std::optional<ULONGLONG>* lost_ancestor) const {
  const Mft::MftTree::Entry* entry = impl_->table.Find(record);
  const Mft::MftTree::Name* name =
      entry != nullptr ? PrimaryName(*entry) : nullptr;
  if (name == nullptr) {
    if (lost_ancestor != nullptr) {
      lost_ancestor->reset();
    }
    return {};
  }
  return impl_->paths.PathThrough(impl_->table, *entry, *name, lost_ancestor);
}

std::wstring
    Mft::MftTree::GetPath(ULONGLONG record, size_t name_index,
                          std::optional<ULONGLONG>* lost_ancestor) const {
  const Mft::MftTree::Entry* entry = impl_->table.Find(record);
  if (entry == nullptr || name_index >= entry->names.size()) {
    if (lost_ancestor != nullptr) {
      lost_ancestor->reset();
    }
    return {};
  }
  // nameIndex < names.size() was checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  return impl_->paths.PathThrough(impl_->table, *entry,
                                  entry->names[name_index], lost_ancestor);
}

std::wstring Mft::MftTree::GetJournalPath(
    size_t index, std::optional<ULONGLONG>* lost_ancestor) const {
  if (lost_ancestor != nullptr) {
    lost_ancestor->reset();
  }
  if (index >= impl_->journal_only.size()) {
    return {};
  }
  // index is checked above.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const Mft::MftTree::JournalOnlyEntry& item = impl_->journal_only[index];

  if (!item.parent_valid) {
    if (lost_ancestor != nullptr) {
      *lost_ancestor = item.parent_record;
    }
    return item.name;
  }
  // The root is always a valid parent, even when its own record is lost.
  if (item.parent_record == root_record) {
    return L"\\" + item.name;
  }

  std::wstring path = GetPath(item.parent_record, lost_ancestor);
  if (path.empty()) {
    if (lost_ancestor != nullptr) {
      *lost_ancestor = item.parent_record;
    }
    return item.name;
  }
  if (path.back() != L'\\') {
    path += L'\\';
  }
  return path + item.name;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
