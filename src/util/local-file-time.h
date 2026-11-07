#pragma once

#include <ntfs-browser/win-types.h>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Util {

// Converts a UTC FILETIME value (100 ns ticks since 1601) to local time. Where
// the system cannot convert it, and on platforms with no timezone conversion
// in the library, local is the UTC value unchanged.
void UtcToLocalFileTime(ULONGLONG utc, FILETIME& local) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Util
