#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

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
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::vector<MftRun>
    PlanMftRuns(std::span<const ULONGLONG> slots, ULONGLONG max_gap_slots,
                ULONGLONG max_run_slots);

// Widens each run to the whole clusters of $MFT that hold its slots, and joins
// the runs that then touch or overlap. A read of $MFT goes by whole clusters,
// so the slots added cost nothing more to read: a probe that keeps them finds
// its neighbours later. A run that starts at first_readable_slot or after does
// not widen below it: a read that takes a slot before it fails whole.
// slots_per_cluster under 2 leaves the runs as they are.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::vector<MftRun>
    WidenToClusters(std::vector<MftRun> runs, ULONGLONG slots_per_cluster,
                    ULONGLONG first_readable_slot);

}  // namespace NTFS_BROWSER_NAMESPACE::Io
