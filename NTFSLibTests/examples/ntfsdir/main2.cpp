#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "ntfsdir-core.h"

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return RunNtfsDir<NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>(argc,
                                                                      argv);
}
