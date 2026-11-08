#pragma once

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench {

// Parses the wide command line and runs the engine it names, or all three one
// after the other with a table of what each did. Returns the exit code.
int Run(int argc, wchar_t* argv[]);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench
