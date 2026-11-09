#pragma once

// The ntfsdir command: lists a folder, or a whole volume, with Scan.
// It shows how to use a Scan: set the Options, hand a callback to Run() and
// print what the batches hold. Plain C++, so main.cpp only hands over argv and
// the unit tests drive it without a process.

#include <cstdio>

#include <ntfs-browser/example/ntfsdir/arguments.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir {

// Runs the scan that args describe and writes the listing to out, as UTF-8.
// The percent of the scan goes to err, with --progress. The log goes to its
// own sinks: the caller configures them. Returns 0 when the scan finishes and
// failure_exit_code otherwise. The totals line comes last, and only when the
// scan finishes: a scan that fails says why in the log.
int ListTarget(const Arguments& args, std::FILE* out, std::FILE* err);

// Parses argv, configures the log, then lists. The usage goes to stdout for
// --help and to stderr, after the reason, for a bad command line. Returns the
// exit code.
int RunNtfsDir(int argc, wchar_t* argv[]);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir
