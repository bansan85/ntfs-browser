#include <ntfs-browser/cache/strategy.h>

#include "ntfsdir-core.h"

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return RunNtfsDir<NtfsBrowser::Cache::Strategy::NoCache>(argc, argv);
}
