#pragma once

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
// A test lowers it to make base records read their extension records from the
// disk. nullopt restores the default.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    SetMaxHeldRecordBytesForTests(std::optional<size_t> limit);

// The cap on held record bytes: the default, or the value a test set.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY size_t MaxHeldRecordBytes();

}  // namespace NTFS_BROWSER_NAMESPACE::Mft
