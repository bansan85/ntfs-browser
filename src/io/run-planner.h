#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "io/read-ahead-held.h"

namespace NTFS_BROWSER_NAMESPACE {

template <Cache::Strategy S>
class NtfsVolume;

namespace Io {

template <Cache::Strategy S>
class FileRecord;

struct ProbeRunPlan;

// One read of $MFT that the probes of a held directory will make.
struct PlannedRun {
  ULONGLONG first_slot;
  ULONGLONG count;
  // The generation of the zone that last took the run.
  std::uint64_t stamp{0};
  // The records it reads for, as a span of RunPlan::wanted.
  std::uint32_t wanted_begin{0};
  std::uint32_t wanted_end{0};
  // Waiting: not on the lane. Announced: on it. Done: read, or not needed.
  enum class State : std::uint8_t { Waiting, Announced, Done };
  State state{State::Waiting};

  // Whether the run is exactly the slots first to first + records - 1.
  [[nodiscard]] bool Spans(ULONGLONG first, ULONGLONG records) const noexcept {
    return first_slot == first && count == records;
  }
};

// The probe runs of one held directory, in the order the walk reads them.
struct RunPlan {
  std::vector<PlannedRun> runs;
  // The records the runs read for, run after run.
  std::vector<ULONGLONG> wanted;
  // The runs before it are Done.
  std::size_t cursor{0};
};

// Works out the probe runs of $MFT that the walk of a held directory will
// make, as the prober cuts them. Reads, logs and announces nothing.
template <Cache::Strategy S>
class RunPlanner {
 public:
  // The planner follows the rules of volume, and finds the records the window
  // or the memo of keeper holds, and the blocks store holds. Everything MUST
  // outlive it.
  RunPlanner(const NtfsVolume<S>& volume, RecordKeeper& keeper,
             const HeldStore& store);

  // The runs the probes of directory will make, from the held part of its
  // index, which record holds as a file record that has been parsed. The
  // blocks come from the held blocks of directory.
  [[nodiscard]] RunPlan PlanFor(ULONGLONG directory, FileRecord<S>& record);

 private:
  // What the probes of a directory read, as they will cut it: for each group
  // of records, in order, the runs of each chunk.
  [[nodiscard]] RunPlan
      PlanOf(const std::vector<std::vector<ULONGLONG>>& groups);

  // Adds the runs that chunk_plan reads to plan, with the records they read
  // for.
  static void AppendRuns(RunPlan& plan, const ProbeRunPlan& chunk_plan);

  const NtfsVolume<S>* volume_;
  RecordKeeper* keeper_;
  const HeldStore* store_;
  Tuning::ProbeTuning probe_;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
