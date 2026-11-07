#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/example/ntfsdir/ntfsdir-core.h>
#include <ntfs-browser/namespace.h>

namespace ExampleNtfsDir = NTFS_BROWSER_NAMESPACE::Example::NtfsDir;

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return ExampleNtfsDir::RunNtfsDir<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>(argc, argv);
}
