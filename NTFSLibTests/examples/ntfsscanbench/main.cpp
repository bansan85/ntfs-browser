// Lists the files of an NTFS volume with one of three strategies, and times
// them. The mftlist engine scans $MFT (MftTree) and lists every file of the
// volume with its path. The nocache and fullcache engines walk the directory
// indexes through NtfsVolume with that cache strategy. With --engine=all, the
// three run on the same target and a table compares them.
// usage: ntfsscanbench [--engine=mftlist|nocache|fullcache|all] [options] path
// eg. ntfsscanbench c:
// eg. ntfsscanbench --engine=all --benchmark=run.json ./disk.img

#include <ntfs-browser/namespace.h>

#include "scan-bench.h"

namespace ExampleNtfsScanBench = NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench;

#ifdef _WIN32

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return ExampleNtfsScanBench::Run(argc, argv);
}

#else

  #include <string>
  #include <vector>

  #include <ntfs-browser/tool/device-access/argv-wide.h>

namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

int main(int argc, char* argv[]) {
  std::vector<std::wstring> wide =
      ToolDeviceAccess::ArgumentsAsWide(argc, argv);
  std::vector<wchar_t*> pointers = ToolDeviceAccess::ArgumentPointers(wide);
  return ExampleNtfsScanBench::Run(argc, pointers.data());
}

#endif
