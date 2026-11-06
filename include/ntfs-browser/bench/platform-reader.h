#pragma once

#include <array>
#include <memory>
#include <string_view>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

namespace NTFS_BROWSER_NAMESPACE::Bench {

// The reader that NtfsVolume opens for a path on this platform, with the given
// read-ahead caps: a Win32DiskReader on Windows, null elsewhere, where the
// tools bring their own. It lets a replay run through the production reader.
[[nodiscard]] NTFS_BROWSER_EXPORT std::unique_ptr<IDiskReader>
    OpenPlatformReader(
        std::wstring_view path,
        const std::array<Tuning::ReaderTuning, Tuning::pattern_count>& tuning);

}  // namespace NTFS_BROWSER_NAMESPACE::Bench
