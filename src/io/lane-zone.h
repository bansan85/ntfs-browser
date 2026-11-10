#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "io/run-planner.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

// A run of the zone, with the plan that owns it.
struct ZoneTarget {
  RunPlan* plan;
  PlannedRun* run;
};

// The runs a ProbeLane looks at: the first 2 * lane_runs of the runs not done,
// in the order Refresh() works them. The plan of the directory being listed
// comes first, then those of the order, a directory twice if the order lists it
// twice. The first lane_runs are the ones the lane should hold, the others the
// ones it may keep. The zone holds pointers into the plans, so the owner MUST
// Forget() it before it adds or drops a plan or changes the order, and MUST
// keep both alive. A run that is done leaves it at the next Refresh(). Not
// thread safe.
class LaneZone {
 public:
  // The zone reads plans and order, which MUST outlive it, and holds at most
  // 2 * lane_runs runs.
  LaneZone(std::unordered_map<ULONGLONG, RunPlan>& plans,
           const std::vector<ULONGLONG>& order, std::size_t lane_runs)
      : plans_(&plans), order_(&order), lane_runs_(lane_runs) {}

  // Forgets the zone and the chain it is taken from. The next Refresh() builds
  // them again.
  void Forget() noexcept;

  // Brings the zone up to date for the directory current: builds it again from
  // the start when Forget() was called or the directory changed, else drops the
  // runs that are done and takes more from the chain. Returns whether it built
  // it again. Only then may a run on the lane have left the zone.
  bool Refresh(std::optional<ULONGLONG> current);

  // The runs of the zone, in lane order.
  [[nodiscard]] std::span<const ZoneTarget> Targets() const { return zone_; }

  // The count of zones built, to tell a run in the zone from one that left
  // (PlannedRun::stamp).
  [[nodiscard]] std::uint64_t Stamp() const { return stamp_; }

  // Whether the zone is what a fresh walk of the plans gives, and, unless
  // rebuilt, every run of announced is in it. Used by the tests.
  [[nodiscard]] bool Matches(std::optional<ULONGLONG> current, bool rebuilt,
                             std::span<PlannedRun* const> announced) const;

 private:
  // Works the zone out from the start, for the directory current.
  void Rebuild(std::optional<ULONGLONG> current);

  // Takes the runs after the zone in the chain, up to 2 * lane_runs.
  void TopUp();

  std::unordered_map<ULONGLONG, RunPlan>* plans_;
  const std::vector<ULONGLONG>* order_;
  std::size_t lane_runs_;
  std::vector<ZoneTarget> zone_;
  // The plans the zone is taken from, in order.
  std::vector<RunPlan*> chain_;
  // Where the zone stopped in the chain: the plan, and the next run in it.
  std::size_t chain_plan_{0};
  std::size_t chain_run_{0};
  // Whether zone_ and chain_ are those of the plans, the order and the
  // directory current_. Until one of them changes, the zone only loses runs
  // that are done.
  bool clean_{false};
  std::optional<ULONGLONG> current_;
  std::uint64_t stamp_{0};
};

}  // namespace NTFS_BROWSER_NAMESPACE::Io
