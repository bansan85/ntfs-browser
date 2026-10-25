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

// Test hook: the volume size and the probe length of a scan that chooses its
// own thread count, in slots, and what the probe decides. A test lowers the
// sizes to make a small image take the choice, and sets verdict to settle it,
// since the measure of a probe on a small image is only noise. nullopt
// restores the default of each: the sizes of a system volume, and the measure.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    SetAutoThreadLimitsForTests(std::optional<ULONGLONG> min_slots,
                                std::optional<ULONGLONG> probe_slots,
                                std::optional<bool> verdict = std::nullopt);

// Whether threads would speed a scan up, given what its first windows took:
// load_seconds spent loading windows, out of elapsed_seconds in all. A scan
// that waits for its volume most of the time is bound by it, and threads only
// add CPU time.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY bool
    ScanThreadsPay(double load_seconds, double elapsed_seconds) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
