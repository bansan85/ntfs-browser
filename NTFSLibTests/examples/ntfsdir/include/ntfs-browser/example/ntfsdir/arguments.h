#pragma once

// The command line of the ntfsdir command, parsed into the options of a
// Scan. Plain C++, so main.cpp only hands over argv and the unit tests
// drive it without a process.

#include <optional>
#include <string>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/volume-target.h>
#include <ntfs-browser/types.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir {

// Exit code of a run that does not end as Finished, and of a bad command line.
inline constexpr int failure_exit_code = 1;

// The parsed command line.
struct Arguments {
  // --log, --deleted, --recover and --help.
  NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess::CommonArguments common;
  // What the scan gets: the target, the engine, --recursive, the pattern and
  // the filter. include_deleted and recover_errors copy the common options.
  NTFS_BROWSER_NAMESPACE::Options scan;
  // --progress: write the percent of the scan to stderr.
  bool progress = false;
};

// The text --help prints.
[[nodiscard]] std::string UsageText();

// Returns nullopt for a command line that cannot run, and says why in error.
// --help needs no target. Otherwise exactly one target is needed. The options
// may come before or after it. A number is made of decimal digits.
std::optional<Arguments> ParseArguments(int argc, wchar_t* argv[],
                                        std::wstring& error);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir
