#pragma once

#include <memory>
#include <string>

#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

// Makes the capturing sink the library's only sink and opens every level
// up, so trace-grade messages reach it. It is installed once before main()
// runs, and both console targets are gone so a test's expected output
// does not also land in the runner's own stdout.
// NTFS_BROWSER_NAMESPACE::Log::Configure() and SetSinks() replace the
// library's sinks wholesale, so a test that calls either MUST call this
// again afterwards.
void InstallCaptureSink();

// The capturing sink itself, for a test that installs it next to a sink of
// its own.
std::shared_ptr<NTFS_BROWSER_NAMESPACE::Log::ISink> CaptureSinkHandle();

// Everything the library has logged since the last call, which this call
// then discards.
std::string TakeCapturedLog();

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
