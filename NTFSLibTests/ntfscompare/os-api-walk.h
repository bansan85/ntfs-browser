#pragma once

#include <filesystem>

#include "entry.h"

namespace NtfsCompare
{

// Recursively lists root via the platform's own native directory-enumeration
// API: FindFirstFileW/FindNextFileW plus GetCompressedFileSizeW on Windows,
// opendir/readdir plus lstat/statx on Linux. This is the reference the other
// passes diff against on that platform, alongside the NtfsBrowser-based
// methods (see compare-engine.h).
[[nodiscard]] Listing WalkOsApi(const std::filesystem::path& root);

// Human-readable name of the OS API this build uses, for the report.
[[nodiscard]] const char* OsApiMethodName() noexcept;

}  // namespace NtfsCompare
