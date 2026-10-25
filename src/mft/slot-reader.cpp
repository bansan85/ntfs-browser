#include "mft/slot-reader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// First slot a window may serve: below it, records are read from a fixed
// address, whether or not $MFT's extents are known.
constexpr ULONGLONG first_mapped_slot = static_cast<ULONGLONG>(Idx::User);

// Windows announced ahead of the one read, at most. A lone used slot makes a
// window of one cluster: the cap keeps the reader's queue short then.
constexpr size_t max_announced_windows = 64;

// Bytes of slots announced ahead of the window read, at most: four default
// windows, as many as the reader keeps in flight.
constexpr ULONGLONG max_announced_bytes = ULONGLONG{16} * 1024 * 1024;

}  // namespace

template <Cache::Strategy S>
SlotReader<S>::SlotReader(const NtfsVolume<S>& volume, size_t block_bytes)
    : volume_(&volume),
      record_size_(volume.GetFileRecordSize()),
      // A volume that is not OK has no record size: the guards keep the
      // division defined, and GetRecordsCount() is 0 then.
      cluster_slots_(
          (std::max<ULONGLONG>)(1, record_size_ != 0
                                       ? volume.GetClusterSize() / record_size_
                                       : 1)),
      window_slots_(record_size_ != 0 ? block_bytes / record_size_ : 1),
      plan_{{.first = 0, .last = volume.GetRecordsCount()}} {
  // A window holds whole clusters, at least one, so the windows of a range all
  // start on a cluster boundary.
  window_slots_ = (std::max)(window_slots_ - window_slots_ % cluster_slots_,
                             cluster_slots_);
}

template <Cache::Strategy S>
SlotReader<S>::~SlotReader() {
  if (announced_any_) {
    volume_->CancelPrefetch();
  }
}

template <Cache::Strategy S>
void SlotReader<S>::Plan(std::vector<SlotRange> ranges) {
  if (announced_any_) {
    volume_->CancelPrefetch();
    announced_any_ = false;
  }
  plan_ = std::move(ranges);
  announced_.clear();
  announced_slots_ = 0;
  next_range_ = 0;
  next_slot_ = 0;
  window_first_ = 0;
  window_last_ = 0;
  failed_first_ = 0;
  failed_last_ = 0;
}

template <Cache::Strategy S>
void SlotReader<S>::Expect(ULONGLONG first, ULONGLONG last) {
  Plan({{.first = first, .last = last}});
}

template <Cache::Strategy S>
bool SlotReader<S>::ParseFileRecord(Io::FileRecord<S>& file_record,
                                    ULONGLONG record) {
  const std::span<const BYTE> bytes = Slot(record);
  if (bytes.empty()) {
    return file_record.ParseFileRecord(record);
  }
  return file_record.ParseFileRecord(record, bytes);
}

template <Cache::Strategy S>
std::span<const BYTE> SlotReader<S>::Slot(ULONGLONG record) {
  if (!ServeWindow(record)) {
    return {};
  }
  return HeldSlot(record);
}

template <Cache::Strategy S>
std::optional<SlotWindow> SlotReader<S>::ServeWindow(ULONGLONG record) {
  if (record < first_mapped_slot) {
    return std::nullopt;
  }
  if (record >= failed_first_ && record < failed_last_) {
    return std::nullopt;
  }
  if (record < window_first_ || record >= window_last_) {
    const size_t range_index = RangeOf(record);
    if (range_index == plan_.size()) {
      return std::nullopt;
    }
    const SlotRange window = WindowOf(plan_.at(range_index), record);
    if (!LoadWindow(range_index, window)) {
      return std::nullopt;
    }
  }

  return SlotWindow{
      .first = window_first_, .last = window_last_, .bytes = window_};
}

template <Cache::Strategy S>
size_t SlotReader<S>::RangeOf(ULONGLONG record) const {
  const auto after = std::ranges::upper_bound(
      plan_, record, std::ranges::less{}, &SlotRange::first);
  if (after == plan_.begin()) {
    return plan_.size();
  }
  const auto index = static_cast<size_t>(std::prev(after) - plan_.begin());
  return record < plan_.at(index).last ? index : plan_.size();
}

template <Cache::Strategy S>
SlotRange SlotReader<S>::WindowOf(const SlotRange& range,
                                  ULONGLONG record) const {
  // The windows of a range follow each other from its first slot's cluster.
  const ULONGLONG first = (std::max)(range.first, first_mapped_slot);
  const ULONGLONG base = first - first % cluster_slots_;
  const ULONGLONG start =
      base + (record - base) / window_slots_ * window_slots_;
  return {.first = (std::max)(start, first),
          .last = range.last - start <= window_slots_ ? range.last
                                                      : start + window_slots_};
}

template <Cache::Strategy S>
std::span<const BYTE> SlotReader<S>::HeldSlot(ULONGLONG record) const {
  // The test of Slot(), but without reading a window.
  if (record < window_first_ || record >= window_last_) {
    return {};
  }
  // record lies in [window_first_, window_last_), and window_ holds exactly
  // those slots.
  const size_t offset =
      gsl::narrow<size_t>((record - window_first_) * record_size_);
  return std::span<const BYTE>{window_}.subspan(offset, record_size_);
}

template <Cache::Strategy S>
bool SlotReader<S>::LoadWindow(size_t range_index, const SlotRange& window) {
  AnnounceFrom(range_index, window);

  // A failed read leaves no window behind, whatever it wrote.
  window_first_ = 0;
  window_last_ = 0;
  window_.resize(
      gsl::narrow<size_t>((window.last - window.first) * record_size_));
  if (!volume_->ReadMftSlots(window.first, window_)) {
    Log::Debug("Block read of $MFT slots [{}, {}) failed; reading them alone",
               window.first, window.last);
    failed_first_ = window.first;
    failed_last_ = window.last;
    return false;
  }

  window_first_ = window.first;
  window_last_ = window.last;
  return true;
}

template <Cache::Strategy S>
void SlotReader<S>::AnnounceFrom(size_t range_index, const SlotRange& window) {
  // The windows up to this one are read now, or were passed over.
  while (!announced_.empty() && announced_.front().first <= window.first) {
    announced_slots_ -= announced_.front().last - announced_.front().first;
    announced_.pop_front();
  }

  // Not announced yet: the caller skipped ahead, or this is the first window.
  // It is read at once, but the reads after it start meanwhile.
  if (next_range_ < range_index ||
      (next_range_ == range_index && next_slot_ <= window.first)) {
    next_range_ = range_index;
    next_slot_ = window.first;
    const SlotRange current = NextWindow();
    volume_->PrefetchMftSlots(current.first, current.last - current.first);
    announced_any_ = true;
  }

  while (announced_.size() < max_announced_windows &&
         announced_slots_ * record_size_ < max_announced_bytes) {
    const SlotRange next = NextWindow();
    if (next.first == next.last) {
      return;
    }
    volume_->PrefetchMftSlots(next.first, next.last - next.first);
    announced_any_ = true;
    announced_.push_back(next);
    announced_slots_ += next.last - next.first;
  }
}

template <Cache::Strategy S>
SlotRange SlotReader<S>::NextWindow() {
  while (next_range_ < plan_.size()) {
    const SlotRange& range = plan_.at(next_range_);
    const ULONGLONG start =
        (std::max)({next_slot_, range.first, first_mapped_slot});
    if (start < range.last) {
      const SlotRange window = WindowOf(range, start);
      next_slot_ = window.last;
      return window;
    }
    next_range_++;
    next_slot_ = 0;
  }
  return {};
}

template class SlotReader<Cache::Strategy::NoCache>;
template class SlotReader<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
