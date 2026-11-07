#pragma once

#include <filesystem>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/ntfscompare/entry.h>

namespace NTFS_BROWSER_NAMESPACE::Tool::NtfsCompare {

// Recursively lists root through std::filesystem::recursive_directory_iterator.
// Only Path/Type/LogicalSize/ModificationTimeUtc are ever filled in: nothing
// else is available through std::filesystem in a portable way.
[[nodiscard]] Listing WalkStdFilesystem(const std::filesystem::path& root);

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::NtfsCompare
