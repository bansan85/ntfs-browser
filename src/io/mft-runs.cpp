#include "io/mft-runs.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

std::vector<MftRun> PlanMftRuns(std::span<const ULONGLONG> slots,
                                ULONGLONG max_gap_slots,
                                ULONGLONG max_run_slots) {
  max_run_slots = (std::max<ULONGLONG>)(1, max_run_slots);
  std::vector<MftRun> runs;
  for (size_t index = 0; index < slots.size(); index++) {
    const ULONGLONG slot = slots[index];
    if (!runs.empty()) {
      MftRun& run = runs.back();
      // The hole between the run and slot is narrow, and the run stays short.
      if (slot - run.last_slot - 1 <= max_gap_slots &&
          slot - run.first_slot < max_run_slots) {
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

}  // namespace NTFS_BROWSER_NAMESPACE::Io
