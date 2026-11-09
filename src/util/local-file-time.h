#pragma once

#include <ntfs-browser/namespace.h>

#include "internal-export.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Util {

// Converts a UTC FILETIME value (100 ns ticks since 1601) to local time. Where
// the system cannot convert it, and on platforms with no timezone conversion
// in the library, local is the UTC value unchanged.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    UtcToLocalFileTime(ULONGLONG utc, FILETIME& local) noexcept;

// The UTC ticks that UtcToLocalFileTime() turned into local. The conversion
// uses one offset for every date, so it can be undone exactly. A local time of
// zero, which stands for a time the volume does not hold, stays zero.
[[nodiscard]] NTFS_BROWSER_EXPORT_TESTS_ONLY ULONGLONG
    LocalToUtcTicks(const FILETIME& local) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Util
