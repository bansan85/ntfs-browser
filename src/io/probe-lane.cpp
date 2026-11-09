#include "io/probe-lane.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "cache/strategy.h"
#include "io/directory-read-ahead-testing.h"
#include "io/directory-read-ahead.h"
#include "io/file-record.h"
#include "io/read-ahead-held.h"
#include "io/run-planner.h"
#include "log/ntfs-common.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Most probe runs the plans of the held directories hold in all (a guard rail,
// not a tunable: a run costs 40 bytes, and 8 for each record it reads for). A
// directory whose runs do not fit is not queued on the standing lane, and its
// probes read as they did.
constexpr std::size_t max_planned_runs = std::size_t{1} << 16U;

// How many unread runs of a directory, from the first, a run that the probes
// have just read is looked for among. The probes read in the order of the plan,
// so the run is the first one or near it: a bound keeps a read that no plan
// names from costing the length of the plan.
constexpr std::size_t run_match_lookahead = 1024;

// Whether Refill() checks its zone against a fresh one, and the checks that
// failed. Set by a test, read at each refill.
std::atomic<bool> verify_lane_zone{false};
std::atomic<std::size_t> lane_zone_mismatches{0};

}  // namespace

template <Cache::Strategy S>
ProbeLane<S>::ProbeLane(const NtfsVolume<S>& volume, ReadAheadShared& shared)
    : volume_(&volume),
      planner_(volume, shared.keeper, shared.store),
      keeper_(&shared.keeper),
      store_(&shared.store),
      capture_(&shared.capture),
      stats_(&shared.stats),
      walk_(volume.GetOptions().tuning.walk),
      lane_runs_(volume.GetMediaKind() == Tuning::MediaKind::Rotational
                     ? walk_.read_ahead_runs_rotational
                     : walk_.read_ahead_runs) {
  if constexpr (S == Cache::Strategy::FullCache) {
    overlap_slots_ = volume.GetFileRecordSize() == 0
                         ? 0
                         : CacheBlockBytes(volume.GetOptions().tuning.cache) /
                               volume.GetFileRecordSize();
  }
}

template <Cache::Strategy S>
bool ProbeLane<S>::On() const noexcept {
  return lane_runs_ != 0 && walk_.read_ahead_bytes != 0 &&
         !volume_->HasAttrRawCB();
}

template <Cache::Strategy S>
void ProbeLane<S>::Attach(FileRecord<S>& listing) {
  listing_ = &listing;
  ForgetZone();
}

template <Cache::Strategy S>
void ProbeLane<S>::Detach() noexcept {
  WithdrawAll();
  listing_ = nullptr;
}

template <Cache::Strategy S>
bool ProbeLane<S>::Overlaps(const PlannedRun& left,
                            const PlannedRun& right) const noexcept {
  // Each run, grown by overlap_slots_ on both sides.
  return left.first_slot < right.first_slot + right.count + overlap_slots_ &&
         right.first_slot < left.first_slot + left.count + overlap_slots_;
}

template <Cache::Strategy S>
bool ProbeLane<S>::HoldsRun(const RunPlan& plan, const PlannedRun& run) {
  if (!keeper_->Enabled()) {
    return false;
  }
  const auto wanted =
      std::span<const ULONGLONG>(plan.wanted)
          .subspan(run.wanted_begin, run.wanted_end - run.wanted_begin);
  return std::ranges::none_of(wanted, [this](ULONGLONG record) {
    return keeper_->FindQuietly(record).empty();
  });
}

template <Cache::Strategy S>
void ProbeLane<S>::PlanRunsOf(ULONGLONG directory, FileRecord<S>& record) {
  if (!On() || plans_.contains(directory)) {
    return;
  }
  try {
    RunPlan plan = planner_.PlanFor(directory, record);
    // A plan that does not fit is left out: the probes of that directory read
    // as they did.
    if (plan.runs.empty() ||
        planned_runs_ + plan.runs.size() > max_planned_runs) {
      return;
    }
    planned_runs_ += plan.runs.size();
    stats_->runs_planned += plan.runs.size();
    AddPlan(directory, std::move(plan));
  } catch (const std::exception&) {
    // A plan that fails costs the gain only.
  }
}

template <Cache::Strategy S>
void ProbeLane<S>::Reorder(std::span<const ULONGLONG> upcoming) {
  const std::size_t window = (std::min)(upcoming.size(), walk_.read_ahead_dirs);
  std::vector<ULONGLONG> order(
      upcoming.begin(), upcoming.begin() + gsl::narrow<std::ptrdiff_t>(window));
  const std::unordered_set<ULONGLONG> top(order.begin(), order.end());
  std::ranges::copy_if(
      order_, std::back_inserter(order), [this, &top](ULONGLONG directory) {
        return plans_.contains(directory) && !top.contains(directory);
      });
  SetOrder(std::move(order));
  Refill();
}

template <Cache::Strategy S>
void ProbeLane<S>::Release(ULONGLONG directory) {
  // A directory whose index sits in its record holds no bytes of its own, and
  // has a plan all the same.
  const auto plan = plans_.find(directory);
  if (plan != plans_.end()) {
    WithdrawPlan(plan->second);
    planned_runs_ -= plan->second.runs.size();
    DropPlan(plan);
  }
}

template <Cache::Strategy S>
void ProbeLane<S>::SendHint(const PlannedRun& run, bool announce) noexcept {
  try {
    // The hint of a forged slot MUST NOT log.
    capture_->Reset();
    const Log::Detail::CaptureScope scope(capture_->log);
    if (announce) {
      volume_->PrefetchMftSlotsStanding(run.first_slot, run.count);
    } else {
      volume_->CancelMftSlotsStanding(run.first_slot, run.count);
    }
  } catch (const std::exception&) {
    // A hint that fails costs the gain only.
  }
}

template <Cache::Strategy S>
void ProbeLane<S>::ForgetUnannounced() noexcept {
  std::erase_if(announced_, [](const PlannedRun* run) {
    return run->state != PlannedRun::State::Announced;
  });
}

template <Cache::Strategy S>
void ProbeLane<S>::WithdrawPlan(RunPlan& plan) noexcept {
  for (PlannedRun& run : plan.runs) {
    if (run.state == PlannedRun::State::Announced) {
      SendHint(run, false);
      stats_->runs_withdrawn++;
    }
    run.state = PlannedRun::State::Done;
  }
  ForgetUnannounced();
}

template <Cache::Strategy S>
void ProbeLane<S>::WithdrawAll() noexcept {
  for (const PlannedRun* const run : announced_) {
    SendHint(*run, false);
    stats_->runs_withdrawn++;
  }
  announced_.clear();
  ForgetZone();
  plans_.clear();
  planned_runs_ = 0;
}

template <Cache::Strategy S>
std::optional<std::size_t> ProbeLane<S>::FindReadRun(const RunPlan& plan,
                                                     ULONGLONG first_slot,
                                                     ULONGLONG count) const {
  // The probes read in the order of the plan, so the run is the first one
  // that is not done, or near it, and is the plan's run or a part of it.
  const std::size_t end =
      (std::min)(plan.runs.size(), plan.cursor + run_match_lookahead);
  for (std::size_t index = plan.cursor; index < end; index++) {
    const PlannedRun& run = plan.runs.at(index);
    if (run.state != PlannedRun::State::Done && run.first_slot <= first_slot &&
        first_slot + count <= run.first_slot + run.count) {
      return index;
    }
  }
  return std::nullopt;
}

template <Cache::Strategy S>
bool ProbeLane<S>::TookWhole(const PlannedRun& run, bool read,
                             ULONGLONG first_slot, ULONGLONG count) noexcept {
  // A FullCache reads by blocks, and may not reach the end of the range.
  return S == Cache::Strategy::NoCache && read && run.Spans(first_slot, count);
}

template <Cache::Strategy S>
void ProbeLane<S>::RetireRun(PlannedRun& run, bool read, ULONGLONG first_slot,
                             ULONGLONG count) {
  if (run.state == PlannedRun::State::Announced &&
      !TookWhole(run, read, first_slot, count)) {
    SendHint(run, false);
    // The run that was read is not counted as withdrawn unread.
    if (!read) {
      stats_->runs_withdrawn++;
    }
  }
  run.state = PlannedRun::State::Done;
}

template <Cache::Strategy S>
void ProbeLane<S>::MarkRead(RunPlan& plan, std::size_t index,
                            ULONGLONG first_slot, ULONGLONG count) {
  // A run before it was not read: the probes found its records elsewhere, in
  // the window. The run itself is read, and what a smaller read left of it
  // is withdrawn. A FullCache reads by blocks and skips those its cache holds:
  // it may not reach the end of the range, which the reader then keeps.
  for (std::size_t skipped = plan.cursor; skipped <= index; skipped++) {
    RetireRun(plan.runs.at(skipped), skipped == index, first_slot, count);
  }
}

template <Cache::Strategy S>
void ProbeLane<S>::RunRead(ULONGLONG first_slot, ULONGLONG count) {
  if (!On() || listing_ == nullptr) {
    return;
  }
  const std::optional<ULONGLONG> directory = listing_->GetFileReference();
  const auto found = directory ? plans_.find(*directory) : plans_.end();
  if (found == plans_.end()) {
    stats_->runs_unplanned++;
    return;
  }
  RunPlan& plan = found->second;
  const std::optional<std::size_t> index = FindReadRun(plan, first_slot, count);
  if (!index) {
    stats_->runs_unplanned++;
    return;
  }
  MarkRead(plan, *index, first_slot, count);
  stats_->runs_read++;
  ForgetUnannounced();
  plan.cursor = *index + 1;
  while (plan.cursor < plan.runs.size() &&
         plan.runs.at(plan.cursor).state == PlannedRun::State::Done) {
    plan.cursor++;
  }
  Refill();
}

template <Cache::Strategy S>
void ProbeLane<S>::SetOrder(std::vector<ULONGLONG> order) noexcept {
  order_ = std::move(order);
  ForgetZone();
}

template <Cache::Strategy S>
void ProbeLane<S>::AddPlan(ULONGLONG directory, RunPlan plan) {
  plans_.emplace(directory, std::move(plan));
  ForgetZone();
}

template <Cache::Strategy S>
void ProbeLane<S>::DropPlan(
    std::unordered_map<ULONGLONG, RunPlan>::iterator plan) noexcept {
  plans_.erase(plan);
  ForgetZone();
}

template <Cache::Strategy S>
void ProbeLane<S>::ForgetZone() noexcept {
  zone_clean_ = false;
  zone_.clear();
  chain_.clear();
}

template <Cache::Strategy S>
void ProbeLane<S>::RebuildZone(std::optional<ULONGLONG> current) {
  ForgetZone();
  zone_stamp_++;
  chain_plan_ = 0;
  chain_run_ = 0;
  const auto chain = [this](ULONGLONG directory) {
    const auto found = plans_.find(directory);
    if (found != plans_.end()) {
      chain_.push_back(&found->second);
    }
  };
  if (current) {
    chain(*current);
  }
  for (const ULONGLONG directory : order_) {
    if (directory != current) {
      chain(directory);
    }
  }
  zone_current_ = current;
  zone_clean_ = true;
  TopUpZone();
}

template <Cache::Strategy S>
void ProbeLane<S>::TopUpZone() {
  while (zone_.size() < 2 * lane_runs_ && chain_plan_ < chain_.size()) {
    RunPlan& plan = *chain_[chain_plan_];
    // The runs before the cursor are done, and so are those it jumped over.
    chain_run_ = (std::max)(chain_run_, plan.cursor);
    if (chain_run_ >= plan.runs.size()) {
      chain_plan_++;
      chain_run_ = 0;
      continue;
    }
    PlannedRun& run = plan.runs.at(chain_run_++);
    if (run.state != PlannedRun::State::Done) {
      run.stamp = zone_stamp_;
      zone_.push_back({.plan = &plan, .run = &run});
    }
  }
}

template <Cache::Strategy S>
void ProbeLane<S>::VerifyZone(std::optional<ULONGLONG> current,
                              bool rebuilt) const {
  // The zone as a fresh walk of the plans gives it.
  std::vector<const PlannedRun*> fresh;
  const auto take = [this, &fresh](ULONGLONG directory) {
    const auto found = plans_.find(directory);
    if (found == plans_.end()) {
      return;
    }
    const RunPlan& plan = found->second;
    for (std::size_t index = plan.cursor;
         index < plan.runs.size() && fresh.size() < 2 * lane_runs_; index++) {
      if (plan.runs.at(index).state != PlannedRun::State::Done) {
        fresh.push_back(&plan.runs.at(index));
      }
    }
  };
  if (current) {
    take(*current);
  }
  for (const ULONGLONG directory : order_) {
    if (directory != current) {
      take(directory);
    }
  }
  bool same = std::ranges::equal(
      fresh, zone_, [](const PlannedRun* left, const Target& right) {
        return left == right.run;
      });
  // Without a rebuild, no run of the lane may have left the zone.
  for (const PlannedRun* const run : announced_) {
    same = same && (rebuilt || run->stamp == zone_stamp_);
  }
  if (!same) {
    lane_zone_mismatches++;
    throw std::logic_error("the lane zone differs from a fresh one");
  }
}

template <Cache::Strategy S>
void ProbeLane<S>::WithdrawLeftOut() {
  for (PlannedRun* const run : announced_) {
    if (run->stamp != zone_stamp_) {
      SendHint(*run, false);
      run->state = PlannedRun::State::Waiting;
      stats_->runs_withdrawn++;
    }
  }
  ForgetUnannounced();
}

template <Cache::Strategy S>
void ProbeLane<S>::AnnounceZone() {
  // The first lane_runs_ runs of the zone that wait are announced, but for
  // those whose records the window holds by now, and for those that overlap a
  // run on the lane: a withdrawal takes every range that overlaps it, so the
  // ranges on the lane are kept apart. Such a run waits until the other one is
  // read.
  const std::size_t target = (std::min)(zone_.size(), lane_runs_);
  for (std::size_t index = 0; index < target; index++) {
    const Target entry = zone_.at(index);
    PlannedRun& run = *entry.run;
    if (run.state != PlannedRun::State::Waiting) {
      continue;
    }
    if (HoldsRun(*entry.plan, run)) {
      run.state = PlannedRun::State::Done;
      stats_->runs_window_held++;
      continue;
    }
    if (std::ranges::any_of(announced_, [this, &run](const PlannedRun* other) {
          return Overlaps(*other, run);
        })) {
      continue;
    }
    SendHint(run, true);
    run.state = PlannedRun::State::Announced;
    announced_.push_back(&run);
    stats_->runs_announced++;
  }
}

template <Cache::Strategy S>
void ProbeLane<S>::Refill() {
  if (!On() || plans_.empty()) {
    return;
  }

  std::optional<ULONGLONG> current;
  if (listing_ != nullptr) {
    current = listing_->GetFileReference();
  }
  const bool rebuilt = !zone_clean_ || current != zone_current_;
  if (rebuilt) {
    RebuildZone(current);
  } else {
    std::erase_if(zone_, [](const Target& entry) {
      return entry.run->state == PlannedRun::State::Done;
    });
    TopUpZone();
  }
  if (verify_lane_zone.load(std::memory_order_relaxed)) {
    VerifyZone(current, rebuilt);
  }

  // The runs that left the zone are withdrawn, and may come back. Nothing
  // leaves it but by a rebuild: the zone only loses the runs that are done.
  if (rebuilt) {
    WithdrawLeftOut();
  }
  AnnounceZone();
}

void SetVerifyLaneZoneForTests(bool verify) noexcept {
  verify_lane_zone = verify;
}

std::size_t LaneZoneMismatchesForTests() noexcept {
  return lane_zone_mismatches;
}

template class ProbeLane<Cache::Strategy::NoCache>;
template class ProbeLane<Cache::Strategy::FullCache>;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
