#include "mft/waiting-reads.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>

#include "bench/recorder.h"
#include "mft/held-records.h"
#include "mft/tree-testing.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// The interval a test asked for. nullopt means the default.
std::optional<size_t> announce_restart_interval_override;

}  // namespace

void SetAnnounceRestartIntervalForTests(std::optional<size_t> records) {
  announce_restart_interval_override = records;
}

template <Cache::Strategy S>
UnheldExtensions<S>::UnheldExtensions(const NtfsVolume<S>& volume)
    : volume_(&volume),
      record_size_(volume.GetFileRecordSize()),
      max_preread_(volume.GetOptions().tuning.mft.max_preread_extensions),
      max_run_(
          (std::max<size_t>)(1, volume.GetOptions().tuning.mft.max_run_slots)) {
}

template <Cache::Strategy S>
void UnheldExtensions<S>::Load(const std::vector<ULONGLONG>& records) {
  Clear();
  const size_t count = (std::min)(records.size(), max_preread_);
  for (size_t first = 0; first < count;) {
    size_t run = 1;
    // The run goes on while the next record directly follows, up to its cap.
    while (first + run < count && run < max_run_ &&
           records.at(first + run) == records.at(first) + run) {
      run++;
    }
    Read(records.at(first), run);
    first += run;
  }
}

template <Cache::Strategy S>
std::span<const BYTE> UnheldExtensions<S>::Find(ULONGLONG record) const {
  const auto found = offsets_.find(record);
  if (found == offsets_.end()) {
    return {};
  }
  return std::span<const BYTE>{bytes_}.subspan(found->second, record_size_);
}

template <Cache::Strategy S>
void UnheldExtensions<S>::Clear() noexcept {
  bytes_.clear();
  offsets_.clear();
}

template <Cache::Strategy S>
void UnheldExtensions<S>::Read(ULONGLONG first, size_t run) {
  const size_t start = bytes_.size();
  bytes_.resize(start + run * record_size_);
  const std::span<BYTE> out =
      std::span<BYTE>{bytes_}.subspan(start, run * record_size_);
  const Bench::Detail::KindScope kind(Bench::ReadKind::ExtensionRecord);
  if (!volume_->ReadMftSlots(first, out)) {
    bytes_.resize(start);
    return;
  }
  for (size_t i = 0; i < run; i++) {
    offsets_.emplace(first + i, start + i * record_size_);
  }
}

template <Cache::Strategy S>
ReadAnnouncer<S>::ReadAnnouncer(
    const NtfsVolume<S>& volume,
    const std::vector<HeldRecords::Waiting>& waiting)
    : volume_(&volume),
      waiting_(&waiting),
      tuning_(&volume.GetOptions().tuning.mft),
      ranges_(waiting.size(), 0) {}

template <Cache::Strategy S>
void ReadAnnouncer<S>::Before(size_t index) {
  for (; passed_ < index; passed_++) {
    if (ranges_.at(passed_) != 0) {
      ahead_records_--;
      ahead_ranges_ -= ranges_.at(passed_);
      passed_with_reads_++;
    }
  }
  // Another record's reads fit: neither cap is reached.
  for (; next_ < waiting_->size() &&
         ahead_records_ < tuning_->max_announced_records &&
         ahead_ranges_ < tuning_->max_announced_ranges;
       next_++) {
    const HeldRecords::Waiting& waiting = waiting_->at(next_);
    if (!HasReads(waiting)) {
      continue;
    }
    if (!cancelled_) {
      // A first pass that stopped early leaves blocks announced, which
      // would be read for nothing, and before these.
      volume_->CancelPrefetch();
      cancelled_ = true;
    }
    const size_t announced = Announce(waiting);
    ranges_.at(next_) = announced;
    ahead_records_++;
    ahead_ranges_ += announced;
  }
}

template <Cache::Strategy S>
bool ReadAnnouncer<S>::RestartDue() const noexcept {
  return passed_with_reads_ >=
         (std::max)(size_t{1}, announce_restart_interval_override.value_or(
                                   tuning_->announce_restart_interval));
}

template <Cache::Strategy S>
void ReadAnnouncer<S>::Restart(size_t index) {
  volume_->CancelPrefetch();
  passed_with_reads_ = 0;
  std::ranges::fill(ranges_, size_t{0});
  passed_ = index;
  next_ = index;
  ahead_records_ = 0;
  ahead_ranges_ = 0;
}

template <Cache::Strategy S>
bool ReadAnnouncer<S>::HasReads(const HeldRecords::Waiting& waiting) {
  return !waiting.slot || !waiting.list_extents.empty() ||
         !waiting.unheld_extensions.empty();
}

template <Cache::Strategy S>
size_t ReadAnnouncer<S>::Announce(const HeldRecords::Waiting& waiting) const {
  size_t announced = 0;
  if (!waiting.slot) {
    volume_->PrefetchMftSlots(waiting.record, 1);
    announced++;
  }
  // Neighbouring records are one read.
  const std::vector<ULONGLONG>& unheld = waiting.unheld_extensions;
  const size_t count =
      (std::min)(unheld.size(), tuning_->max_preread_extensions);
  const size_t max_run = (std::max<size_t>)(1, tuning_->max_run_slots);
  for (size_t first = 0; first < count;) {
    size_t run = 1;
    // As in UnheldExtensions::Load(): the same runs, so the same reads.
    while (first + run < count && run < max_run &&
           unheld.at(first + run) == unheld.at(first) + run) {
      run++;
    }
    volume_->PrefetchMftSlots(unheld.at(first), run);
    announced++;
    first += run;
  }
  const ULONGLONG cluster_size = volume_->GetClusterSize();
  for (const Alloc::Extent& extent : waiting.list_extents) {
    volume_->Prefetch(extent.lcn * cluster_size,
                      extent.clusters * cluster_size);
    announced++;
  }
  return announced;
}

template class UnheldExtensions<Cache::Strategy::NoCache>;
template class UnheldExtensions<Cache::Strategy::FullCache>;
template class ReadAnnouncer<Cache::Strategy::NoCache>;
template class ReadAnnouncer<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
