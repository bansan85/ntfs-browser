#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

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

// A second, wider hole that a run MAY read across, to save a seek on a hard
// disk, while the run stays dense.
struct WideGap {
  // The most free slots of such a hole. At most max_gap_slots: none.
  ULONGLONG gap_slots{0};
  // The run, with the hole, MUST keep this many wanted slots in each thousand
  // of the slots it spans. It bounds the bytes read for nothing, and the bytes
  // that the next directory reads again.
  unsigned min_share_per_mille{0};
};

// Groups sorted, distinct slots into runs of one read. A run reads across a
// hole of at most max_gap_slots free slots, and spans max_run_slots slots at
// most, its first and last included (1 at least). A hole of up to
// wide.gap_slots is read across too, when the run it joins still holds
// wide.min_share_per_mille wanted slots per thousand. The probes of a directory
// walk and the read ahead of a recursive listing plan their reads of $MFT with
// it.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY std::vector<MftRun>
    PlanMftRuns(std::span<const ULONGLONG> slots, ULONGLONG max_gap_slots,
                ULONGLONG max_run_slots, const WideGap& wide = {});

// The wide gap of the probes of a volume read from a device of this kind, with
// records of record_size bytes. Only a rotational device has one: a seek costs
// about as much as a megabyte there, and next to nothing on flash, where the
// narrow gap is already the minimum. And only a volume that keeps what it
// read (cached): a run over a wide hole holds a few records of a big
// directory, and the next node reads the same bytes again, which costs ten
// times a seek on a hard disk when nothing keeps them.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY WideGap
    WideGapOf(const Tuning::ProbeTuning& probe, Tuning::MediaKind kind,
              ULONGLONG record_size, bool cached) noexcept;

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
