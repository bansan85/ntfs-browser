#pragma once

#include <string>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Attaches the capturing sink to the library logger and opens every level
// up, so trace-grade messages reach it. It is installed once before main()
// runs, and both console targets are silenced so a test's expected output
// does not also land in the runner's own stdout.
// NTFS_BROWSER_NAMESPACE::Log::Configure() replaces the logger's sinks
// wholesale, so a test that calls it MUST call this again afterwards.
void InstallCaptureSink();

// Everything the library has logged since the last call, which this call
// then discards.
std::string TakeCapturedLog();

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
