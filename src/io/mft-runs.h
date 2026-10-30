#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Io {

// One read of consecutive slots of $MFT, holes included.
struct MftRun {
  // The slots the run serves, as indices of the sorted list it was planned
  // from.
  size_t begin;
  size_t end;
  ULONGLONG first_slot;
  ULONGLONG last_slot;
};

// Groups sorted, distinct slots into runs of one read. A run reads across a
// hole of at most max_gap_slots free slots, and spans max_run_slots slots at
// most, its first and last included (1 at least). The probes of a directory
// walk and the read ahead of a recursive listing plan their reads of $MFT with
// it.
[[nodiscard]] std::vector<MftRun> PlanMftRuns(std::span<const ULONGLONG> slots,
                                              ULONGLONG max_gap_slots,
                                              ULONGLONG max_run_slots);

}  // namespace NTFS_BROWSER_NAMESPACE::Io
