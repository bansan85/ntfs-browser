#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Mft {

// Test hook: caps the data-state queries a scan keeps pending before it
// resolves them against the cluster bitmap. A test lowers it to force several
// flushes. 0 restores the default.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SetMaxPendingRangesForTests(size_t limit);

// The cap a scan flushes at: the default, or the value a test set.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t MaxPendingRanges();

// Test hook: caps the units of path that the directory path memo of a tree
// holds, in all. A test lowers it to make a small tree meet the cap. nullopt
// restores the default.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    SetMaxMemoizedPathUnitsForTests(std::optional<size_t> units);

// Test hook: the most units of path that any tree of this thread has held in
// its directory path memo, since the last call of this function with true. The
// count is reset when reset is true, after it is read.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t
    PeakMemoizedPathUnitsForTests(bool reset = false) noexcept;

// Test hook: caps the bytes of raw records a scan holds for its second pass.
// A test lowers it to make base records wait without their bytes, and
// extension records go unheld: the second pass then reads them from the disk.
// nullopt restores the default.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    SetMaxHeldRecordBytesForTests(std::optional<size_t> limit);

// The cap on held record bytes: the default, or the value a test set.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t MaxHeldRecordBytes();

// Test hook: how many record slots the scans of this thread took from the
// threads that parsed them, since the last call of this function with true.
// A scan with one thread takes none: a test reads it to know that the others
// were used. The count is reset when reset is true, after it is read.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t
    SlotsParsedByThreadsForTests(bool reset = false) noexcept;

// Test hook: how many waiting base records the second pass of the scans of this
// thread took from the threads that parsed them, since the last call of this
// function with true. A scan with one thread takes none.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t
    WaitingParsedByThreadsForTests(bool reset = false) noexcept;

// Test hook: how many waiting base records the threads parse in one batch of
// the second pass. A test lowers it to make a scan meet several batches. 0
// would make no progress: it is read as 1. nullopt restores the default.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    SetWaitingBatchRecordsForTests(std::optional<size_t> records);

// Test hook: how many waiting base records with announced reads the second
// pass parses between two restarts of its announcements. A test lowers it to
// make a small image restart. 0 is read as 1. nullopt restores the default.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    SetAnnounceRestartIntervalForTests(std::optional<size_t> records);

// Test hook: the volume size and the probe length of a scan that chooses its
// own thread count, in slots, what the probe decides, and the count of waiting
// base records from which its second pass takes threads. A test lowers the
// sizes to make a small image take the choice, and sets verdict to settle it,
// since the measure of a probe on a small image is only noise. nullopt
// restores the default of each: the sizes of a system volume, the measure, and
// the count of a system volume.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SetAutoThreadLimitsForTests(
    std::optional<ULONGLONG> min_slots, std::optional<ULONGLONG> probe_slots,
    std::optional<bool> verdict = std::nullopt,
    std::optional<size_t> min_waiting = std::nullopt);

// Whether threads would speed a scan up, given what its first windows took:
// load_seconds spent loading windows, out of elapsed_seconds in all. A scan
// that waits for its volume most of the time is bound by it, and threads only
// add CPU time.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY bool
    ScanThreadsPay(double load_seconds, double elapsed_seconds) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
