#pragma once

#include <string>

namespace NtfsBrowserTests
{

// Attaches the capturing sink to the library logger and opens every level
// up, so trace-grade messages reach it. It is installed once before main()
// runs, and both console targets are silenced so a test's expected output
// does not also land in the runner's own stdout.
// NtfsBrowser::Log::Configure() replaces the logger's sinks wholesale, so
// a test that calls it MUST call this again afterwards.
void InstallCaptureSink();

// Everything the library has logged since the last call, which this call
// then discards.
std::string TakeCapturedLog();

}  // namespace NtfsBrowserTests
