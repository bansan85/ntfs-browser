#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>

#include "alloc/claim-inventory.h"
#include "alloc/cluster-sweep.h"
#include "log/ntfs-common.h"
#include "mft/slot-entry.h"
#include "mft/slot-scan.h"
#include "mft/tree-impl.h"
#include "mft/used-slots.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// Entries a scan reserves room for at most, from the count the $MFT bitmap
// gives. 2^20 covers most volumes, and bounds what a forged bitmap makes a
// scan allocate before it reads a record: about 200 MiB.
constexpr ULONGLONG max_reserved_entries = ULONGLONG{1} << 20;

// The level of cluster accounting a scan runs at: what the options ask for, at
// least Counts when the content check is on, and Off when no entry can be
// concerned because the volume keeps neither freed nor damaged records.
template <Cache::Strategy S>
Alloc::ClusterAccountLevel
    EffectiveAccountLevel(const NtfsVolume<S>& volume,
                          const Mft::MftTree::ScanOptions& options) {
  if (!volume.GetOptions().include_deleted &&
      !volume.GetOptions().recover_errors) {
    return Alloc::ClusterAccountLevel::Off;
  }
  if (options.cluster_account == Alloc::ClusterAccountLevel::Off &&
      options.verify_data != Alloc::DataCheckMode::Off) {
    return Alloc::ClusterAccountLevel::Counts;
  }
  return options.cluster_account;
}

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

// Reads the record slots of volume's $MFT, keeps each base record's entry,
// then links the entries into a tree. With include_deleted off, the slots
// $MFT's $BITMAP marks free are not read.
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
  entries.reserve(gsl::narrow<size_t>(
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
    Link();
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
      ScanSlots(volume, input, entries, stats, inventory);
  // A run told to stop after a phase leaves a tree that is not complete.
  if (Bench::StopRequested()) {
    stats.complete = false;
    return std::nullopt;
  }
  for (size_t index = 0; index < entries.size(); index++) {
    // index < entries.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    IndexEntry(entries[index].record, index);
  }
  if (input.features.account && stats.complete) {
    const Bench::PhaseScope phase(Bench::Phase::ClusterSweep);
    volume.SetReadPattern(Tuning::Pattern::LargeSequential);
    AccountClusters(volume, input.options, inventory);
  }
  if (Bench::StopRequested()) {
    stats.complete = false;
    return std::nullopt;
  }
  return journal_extents;
}

template <Cache::Strategy S>
void Mft::MftTree::Impl::AccountClusters(
    const NtfsVolume<S>& volume, const Mft::MftTree::ScanOptions& options,
    Alloc::ClaimInventory& inventory) {
  std::vector<Alloc::OwnerAccount> owners = Alloc::AccountClusters(
      volume, inventory, EffectiveAccountLevel(volume, options),
      options.verify_data);
  accounts.reserve(owners.size());
  for (Alloc::OwnerAccount& owner : owners) {
    if (!EntryIndex(owner.record)) {
      continue;
    }
    stats.cluster_accounts++;
    stats.clusters.Add(owner.account.tally);
    accounts.emplace_back(owner.record, std::move(owner.account));
  }
  std::ranges::sort(accounts, {}, &decltype(accounts)::value_type::first);
  LogClusterSummary();
}

void Mft::MftTree::Impl::LogClusterSummary() const {
  Log::Info(
      "MFT scan clusters: {} files counted, {} intact, {} reallocated, {} "
      "bad, {} sparse",
      stats.cluster_accounts,
      stats.clusters.Count(Alloc::ClusterCondition::Intact),
      stats.clusters.Count(Alloc::ClusterCondition::ReallocatedWritten) +
          stats.clusters.Count(Alloc::ClusterCondition::ReallocatedUnwritten),
      stats.clusters.Count(Alloc::ClusterCondition::Bad),
      stats.clusters.Count(Alloc::ClusterCondition::Sparse));
}

// Logs the counters of the scan. The data state counters only mean something
// when the scan annotated the entries.
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

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
