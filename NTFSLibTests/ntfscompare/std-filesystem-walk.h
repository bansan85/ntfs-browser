#pragma once

#include <filesystem>

#include "entry.h"

namespace NtfsCompare {

// Recursively lists root through std::filesystem::recursive_directory_iterator.
// Only Path/Type/LogicalSize/ModificationTimeUtc are ever filled in: nothing
// else is available through std::filesystem in a portable way.
[[nodiscard]] Listing WalkStdFilesystem(const std::filesystem::path& root);

}  // namespace NtfsCompare
