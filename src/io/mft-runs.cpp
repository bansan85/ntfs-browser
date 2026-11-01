#include "io/mft-runs.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Thousandths in a whole, the unit of WideGap::min_share_per_mille.
constexpr ULONGLONG per_mille = 1000;

}  // namespace

std::vector<MftRun> PlanMftRuns(std::span<const ULONGLONG> slots,
                                ULONGLONG max_gap_slots,
                                ULONGLONG max_run_slots, const WideGap& wide) {
  max_run_slots = (std::max<ULONGLONG>)(1, max_run_slots);
  std::vector<MftRun> runs;
  for (size_t index = 0; index < slots.size(); index++) {
    const ULONGLONG slot = slots[index];
    if (!runs.empty()) {
      MftRun& run = runs.back();
      const ULONGLONG hole = slot - run.last_slot - 1;
      const ULONGLONG span = slot - run.first_slot + 1;
      const ULONGLONG wanted = index + 1 - run.begin;
      // A narrow hole, or a wide one that the run is dense enough to cross,
      // and the run stays short.
      if ((hole <= max_gap_slots ||
           (hole <= wide.gap_slots &&
            wanted * per_mille >= span * wide.min_share_per_mille)) &&
          span <= max_run_slots) {
        run.last_slot = slot;
        run.end = index + 1;
        continue;
      }
    }
    runs.push_back({.begin = index,
                    .end = index + 1,
                    .first_slot = slot,
                    .last_slot = slot});
  }
  return runs;
}

WideGap WideGapOf(const Tuning::ProbeTuning& probe, Tuning::MediaKind kind,
                  ULONGLONG record_size, bool cached) noexcept {
  if (kind != Tuning::MediaKind::Rotational || !cached || record_size == 0 ||
      probe.wide_gap_bytes <= probe.max_gap_bytes) {
    return {};
  }
  return {.gap_slots = probe.wide_gap_bytes / record_size,
          .min_share_per_mille = probe.wide_gap_min_share_per_mille};
}

std::vector<MftRun> WidenToClusters(std::vector<MftRun> runs,
                                    ULONGLONG slots_per_cluster,
                                    ULONGLONG first_readable_slot) {
  if (slots_per_cluster < 2) {
    return runs;
  }
  std::vector<MftRun> widened;
  for (MftRun run : runs) {
    const ULONGLONG aligned =
        run.first_slot - (run.first_slot % slots_per_cluster);
    run.first_slot =
        (std::max)(aligned, (std::min)(run.first_slot, first_readable_slot));
    run.last_slot +=
        slots_per_cluster - 1 - (run.last_slot % slots_per_cluster);
    if (!widened.empty() && run.first_slot <= widened.back().last_slot + 1) {
      MftRun& previous = widened.back();
      previous.last_slot = (std::max)(previous.last_slot, run.last_slot);
      previous.end = run.end;
      continue;
    }
    widened.push_back(run);
  }
  return widened;
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
