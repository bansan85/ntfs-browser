#include "io/lane-zone.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "io/run-planner.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

void LaneZone::Forget() noexcept {
  clean_ = false;
  zone_.clear();
  chain_.clear();
}

bool LaneZone::Refresh(std::optional<ULONGLONG> current) {
  const bool rebuilt = !clean_ || current != current_;
  if (rebuilt) {
    Rebuild(current);
  } else {
    std::erase_if(zone_, [](const ZoneTarget& entry) {
      return entry.run->state == PlannedRun::State::Done;
    });
    TopUp();
  }
  return rebuilt;
}

void LaneZone::Rebuild(std::optional<ULONGLONG> current) {
  Forget();
  stamp_++;
  chain_plan_ = 0;
  chain_run_ = 0;
  const auto chain = [this](ULONGLONG directory) {
    const auto found = plans_->find(directory);
    if (found != plans_->end()) {
      chain_.push_back(&found->second);
    }
  };
  if (current) {
    chain(*current);
  }
  for (const ULONGLONG directory : *order_) {
    if (directory != current) {
      chain(directory);
    }
  }
  current_ = current;
  clean_ = true;
  TopUp();
}

void LaneZone::TopUp() {
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
      run.stamp = stamp_;
      zone_.push_back({.plan = &plan, .run = &run});
    }
  }
}

bool LaneZone::Matches(std::optional<ULONGLONG> current, bool rebuilt,
                       std::span<PlannedRun* const> announced) const {
  // The zone as a fresh walk of the plans gives it.
  std::vector<const PlannedRun*> fresh;
  const auto take = [this, &fresh](ULONGLONG directory) {
    const auto found = plans_->find(directory);
    if (found == plans_->end()) {
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
  for (const ULONGLONG directory : *order_) {
    if (directory != current) {
      take(directory);
    }
  }
  bool same = std::ranges::equal(
      fresh, zone_, [](const PlannedRun* left, const ZoneTarget& right) {
        return left == right.run;
      });
  // Without a rebuild, no run of the lane may have left the zone.
  for (const PlannedRun* const run : announced) {
    same = same && (rebuilt || run->stamp == stamp_);
  }
  return same;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
