#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Tuning {

enum class MediaKind : std::uint8_t;
struct ProbeTuning;

}  // namespace Tuning
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE {

namespace Cache {

enum class Strategy : std::uint8_t;

}  // namespace Cache

template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

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

// What decides the reads of $MFT that the probes of a chunk of records make.
// EntryProber reads exactly the runs that PlanProbeRuns() plans from it, and
// whoever announces those reads ahead MUST plan them from the same rules.
struct ProbeRunRules {
  // Whether $MFT is readable through its extents. When it is not, nothing is
  // read in runs and each record is probed alone.
  bool mapped{false};
  // Size of a file record in bytes.
  ULONGLONG record_size{0};
  // The narrow hole, and the longest run, in slots.
  ULONGLONG max_gap_slots{0};
  ULONGLONG max_run_slots{0};
  WideGap wide;
  // Slots in a cluster of $MFT when a run that is kept widens to whole
  // clusters. Under 2: the runs stay as planned.
  ULONGLONG slots_per_cluster{0};
  // First slot that is read through $MFT's extents: below it, a record has a
  // fixed address and is probed alone.
  ULONGLONG first_mapped_slot{0};
};

// The rules of the probes of volume. keeps is whether something keeps the runs
// the probes read (a ProbeStore with a `keep`): a run that is kept is widened
// to whole clusters.
template <Cache::Strategy S>
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY ProbeRunRules
    ProbeRunRulesOf(const NtfsVolume<S>& volume, bool keeps);

// What PlanProbeRuns() decided for one chunk of records.
struct ProbeRunPlan {
  // How many records of the chunk, the first ones, are probed alone: those
  // below the first mapped slot, and all of them when $MFT is not mapped.
  size_t alone{0};
  // The records of the chunk that no held copy serves, in slot order. A run
  // serves them, as indices of this list.
  std::vector<ULONGLONG> to_read;
  // The reads of $MFT, in the order they are made.
  std::vector<MftRun> runs;
};

// The bytes of a record that something holds, empty when it holds none.
using HeldRecordSource = std::function<std::span<const BYTE>(ULONGLONG record)>;

// What takes a held record: its number, and its bytes, valid for the call.
using HeldRecordSink =
    std::function<void(ULONGLONG record, std::span<const BYTE> bytes)>;

// Plans the reads that the probes of chunk make: sorted, distinct record
// numbers. A record that find gives, in exactly one record size, is held:
// held takes it (it MAY be empty) and no run reads it. find is asked once for
// each record from the first mapped slot on, in order, and not at all when
// $MFT is not mapped or find is empty. The others are read in runs: narrow and
// wide holes, then widened to clusters when rules asks. Reads, logs and
// announces nothing.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY ProbeRunPlan
    PlanProbeRuns(std::span<const ULONGLONG> chunk, const ProbeRunRules& rules,
                  const HeldRecordSource& find, const HeldRecordSink& held);

}  // namespace NTFS_BROWSER_NAMESPACE::Io
