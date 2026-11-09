#include "mft/data-state-resolver.h"

#include <cstddef>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "alloc/data-state.h"
#include "alloc/types.h"
#include "bench/bench.h"
#include "cache/strategy.h"
#include "mft/slot-entry.h"
#include "mft/tree-testing.h"
#include "mft/tree.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

namespace {

// The cap a test asked for. 0 means the default.
size_t max_pending_ranges_override = 0;

}  // namespace

size_t MaxPendingRanges() {
  // The values Tuning::MftTuning has when nothing sets them: the one source of
  // every default. The cap a test asked for sits on top of them.
  static const Tuning::MftTuning defaults;
  return max_pending_ranges_override != 0 ? max_pending_ranges_override
                                          : defaults.max_pending_ranges;
}

void SetMaxPendingRangesForTests(size_t limit) {
  max_pending_ranges_override = limit;
}

template <Cache::Strategy S>
DataStateResolver<S>::DataStateResolver(
    const NtfsVolume<S>& volume, std::vector<Mft::MftTree::Entry>& entries,
    Mft::MftTree::ScanStats& stats, bool keep_windows)
    : volume_(&volume),
      entries_(&entries),
      stats_(&stats),
      keep_windows_(keep_windows),
      max_pending_(max_pending_ranges_override != 0
                       ? max_pending_ranges_override
                       : volume.GetOptions().tuning.mft.max_pending_ranges),
      kept_bitmap_bytes_(volume.GetOptions().tuning.mft.max_kept_bitmap_bytes) {
}

template <Cache::Strategy S>
DataStateResolver<S>::~DataStateResolver() {
  if (keeping_ && !keep_windows_) {
    volume_->KeepBitmapWindows(0);
  }
}

template <Cache::Strategy S>
void DataStateResolver<S>::Add(size_t entry_index, DataNote note) {
  if (note.kind == DataNote::Kind::None) {
    return;
  }
  if (note.kind == DataNote::Kind::Known) {
    Set(entry_index, note.state);
    return;
  }

  waiting_.push_back({.entry_index = entry_index,
                      .first_query = queries_.size(),
                      .query_count = note.extents.extents.size(),
                      .complete = note.extents.complete});
  for (const Alloc::Extent& extent : note.extents.extents) {
    queries_.push_back({.lcn = extent.lcn, .clusters = extent.clusters});
  }
  if (queries_.size() >= max_pending_) {
    Flush();
  }
}

template <Cache::Strategy S>
void DataStateResolver<S>::Finish() {
  Flush();
  if (keeping_ && !keep_windows_) {
    volume_->KeepBitmapWindows(0);
    keeping_ = false;
  }
}

template <Cache::Strategy S>
void DataStateResolver<S>::Flush() {
  if (waiting_.empty()) {
    return;
  }
  // Later batches meet the windows of this one again.
  if (!keeping_) {
    volume_->KeepBitmapWindows(kept_bitmap_bytes_);
    keeping_ = true;
  }
  volume_->SetReadPattern(Tuning::Pattern::ScatteredGrouped);
  {
    const Bench::PhaseScope phase(Bench::Phase::DataStateFlush);
    volume_->GetRangeStates(queries_);
  }

  std::vector<Alloc::ClusterState> states;
  for (const Waiting& entry : waiting_) {
    states.clear();
    for (size_t i = 0; i < entry.query_count; i++) {
      states.push_back(queries_.at(entry.first_query + i).state);
    }
    Set(entry.entry_index, Alloc::FoldDataState(entry.complete, states));
  }
  waiting_.clear();
  queries_.clear();
}

template <Cache::Strategy S>
void DataStateResolver<S>::Set(size_t entry_index, Alloc::DataState state) {
  entries_->at(entry_index).data_state = state;
  switch (state) {
    case Alloc::DataState::Intact:
      stats_->data_intact++;
      break;
    case Alloc::DataState::Reallocated:
      stats_->data_reallocated++;
      break;
    case Alloc::DataState::Unknown:
      stats_->data_unknown++;
      break;
    case Alloc::DataState::Bad:
      stats_->data_bad++;
      break;
  }
}

template class DataStateResolver<Cache::Strategy::NoCache>;
template class DataStateResolver<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
