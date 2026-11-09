#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "cache/strategy.h"
#include "io/directory-read-ahead.h"
#include "io/read-ahead-held.h"
#include "io/run-planner.h"
#include "tuning/tuning.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Io {

template <Cache::Strategy S>
class FileRecord;

// The standing lane of a DirectoryReadAhead: the probe runs of $MFT that the
// held directories will make, and the first few of them kept announced on the
// reader's standing lane. A hint only: no byte more is read.
//
// It plans the runs of a directory once its index is held (PlanRunsOf()), keeps
// the first lane_runs_ unread runs announced, the rest of the directory being
// listed first, then the directories of the stack from the top down, and
// refills as the probes read (RunRead()). Not thread safe.
template <Cache::Strategy S>
class ProbeLane {
 public:
  // The lane reads its limits from volume.GetOptions().tuning.walk, and its
  // records and blocks from shared. Both MUST outlive it.
  ProbeLane(const NtfsVolume<S>& volume, ReadAheadShared& shared);
  ProbeLane(const ProbeLane&) = delete;
  ProbeLane& operator=(const ProbeLane&) = delete;
  ProbeLane(ProbeLane&&) = delete;
  ProbeLane& operator=(ProbeLane&&) = delete;
  ~ProbeLane() = default;

  // Whether the standing lane is in use: a count of runs, a budget that plans
  // something, and no raw callback.
  [[nodiscard]] bool On() const noexcept;

  // Follows the directory listing, whose runs the probes report to RunRead().
  void Attach(FileRecord<S>& listing);

  // Withdraws every announced run, forgets every plan and lets go of the
  // listing.
  void Detach() noexcept;

  // Plans the probe runs of directory from the held part of its index, which
  // record holds as a file record that has been parsed. The blocks come from
  // the held blocks of directory.
  void PlanRunsOf(ULONGLONG directory, FileRecord<S>& record);

  // The stack of the listing changed: the lane follows its first
  // read_ahead_dirs directories. A directory that sank below them keeps its
  // place under the new ones, and its runs stay announced.
  void Reorder(std::span<const ULONGLONG> upcoming);

  // Makes the standing lane hold the first lane_runs_ runs not read yet, the
  // rest of the directory being listed first, then the order of Reorder():
  // announces those that entered that set. A run that left it stays while it is
  // among the next lane_runs_: a stack that grows and shrinks pushes runs out
  // of the first and back in, and a withdrawal costs the read already made and
  // the read again. The others are withdrawn.
  // Those 2 * lane_runs_ runs are kept in zone_ between two calls, and only
  // worked out again when the plans, the order or the listing changed.
  void Refill();

  // The probes have read the run from first_slot on, count records long (the
  // `ran` of the listing's ProbeStore).
  void RunRead(ULONGLONG first_slot, ULONGLONG count);

  // Withdraws the unread runs of directory and forgets its plan.
  void Release(ULONGLONG directory);

 private:
  // A run of the zone, with the plan that owns it.
  struct Target {
    RunPlan* plan;
    PlannedRun* run;
  };

  // Whether the reader could take the two runs for overlapping ranges, which
  // a withdrawal of one would then take with it.
  [[nodiscard]] bool Overlaps(const PlannedRun& left,
                              const PlannedRun& right) const noexcept;

  // Whether the window or the memo holds every record that run reads for.
  [[nodiscard]] bool HoldsRun(const RunPlan& plan, const PlannedRun& run);

  // Sends the hint for run to the reader: announces it on the standing lane,
  // or withdraws it. Never throws: a hint that fails costs the gain only.
  void SendHint(const PlannedRun& run, bool announce) noexcept;

  // Withdraws the runs of a plan that were announced, and forgets them.
  void WithdrawPlan(RunPlan& plan) noexcept;

  // Withdraws every announced run and forgets every plan.
  void WithdrawAll() noexcept;

  // The index of the run of plan that the probes just read, from first_slot on,
  // count records long, or nullopt when no run near the cursor holds it.
  [[nodiscard]] std::optional<std::size_t> FindReadRun(const RunPlan& plan,
                                                       ULONGLONG first_slot,
                                                       ULONGLONG count) const;

  // Whether the read of the probes took run whole, and nothing is left of it
  // to withdraw.
  [[nodiscard]] static bool TookWhole(const PlannedRun& run, bool read,
                                      ULONGLONG first_slot,
                                      ULONGLONG count) noexcept;

  // Marks run done. read is whether the probes read this very run, and not
  // one before it. An announced run that the read did not take whole is
  // withdrawn.
  void RetireRun(PlannedRun& run, bool read, ULONGLONG first_slot,
                 ULONGLONG count);

  // Marks the runs of plan up to index done: the read at index happened, and
  // the earlier ones were not needed. Withdraws what a smaller read left.
  void MarkRead(RunPlan& plan, std::size_t index, ULONGLONG first_slot,
                ULONGLONG count);

  // Drops from announced_ the runs that are not announced any more.
  void ForgetUnannounced() noexcept;

  // The mutations of plans_ and order_: each one forgets the zone.
  void SetOrder(std::vector<ULONGLONG> order) noexcept;
  void AddPlan(ULONGLONG directory, RunPlan plan);
  void DropPlan(std::unordered_map<ULONGLONG, RunPlan>::iterator plan) noexcept;

  // Forgets zone_ and chain_, which hold pointers into plans_.
  void ForgetZone() noexcept;
  // Works the zone out from the start, for the directory current.
  void RebuildZone(std::optional<ULONGLONG> current);
  // Takes the runs after the zone in the chain, up to 2 * lane_runs_.
  void TopUpZone();
  // Throws when zone_ is not what a fresh walk of the plans gives.
  void VerifyZone(std::optional<ULONGLONG> current, bool rebuilt) const;
  // Withdraws the announced runs that the last rebuild left out of the zone.
  void WithdrawLeftOut();
  // Announces the first lane_runs_ runs of the zone that wait.
  void AnnounceZone();

  const NtfsVolume<S>* volume_;
  RunPlanner<S> planner_;
  RecordKeeper* keeper_;
  const HeldStore* store_;
  ReadAheadCapture* capture_;
  DirectoryReadAheadStats* stats_;
  Tuning::WalkTuning walk_;
  FileRecord<S>* listing_{nullptr};

  // Probe runs the standing lane keeps announced, at most: the knob that fits
  // the device. 0 is off.
  std::size_t lane_runs_;
  // The probe runs of each held directory that has any, by record number.
  std::unordered_map<ULONGLONG, RunPlan> plans_;
  // Runs in all in plans_.
  std::size_t planned_runs_{0};
  // Slots that two runs of a FullCache lane must be apart to be sure of
  // disjoint ranges: a block, since the reader's ranges are widened to blocks.
  // 0 under NoCache, whose ranges are the runs themselves.
  ULONGLONG overlap_slots_{0};
  // The runs on the lane now, in the order they were announced. They point
  // into plans_, which a plan leaves only through WithdrawPlan().
  std::vector<PlannedRun*> announced_;
  // The first 2 * lane_runs_ runs not done, in the order Refill() works them:
  // the first lane_runs_ are the ones the lane should hold, the others the
  // ones it may keep. A run that is done leaves it at the next Refill().
  std::vector<Target> zone_;
  // The plans the zone is taken from, in order: the plan of the directory
  // being listed, then those of order_ (a directory twice if order_ lists it
  // twice). The runs of a plan before its cursor are done.
  std::vector<RunPlan*> chain_;
  // Where the zone stopped in the chain: the plan, and the next run in it.
  std::size_t chain_plan_{0};
  std::size_t chain_run_{0};
  // Whether zone_ and chain_ are those of the plans, order_ and the directory
  // zone_current_. Until one of them changes, the zone only loses runs that
  // are done, and every run on the lane is in it.
  bool zone_clean_{false};
  std::optional<ULONGLONG> zone_current_;
  // The count of zones built, to tell a run in the zone from one that left.
  std::uint64_t zone_stamp_{0};
  // The directories of the listing's stack that the lane looks at, the next
  // one first: the top of the stack at the last Reorder(), then the
  // directories that were at the top before and are still planned, in their
  // old order.
  std::vector<ULONGLONG> order_;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
