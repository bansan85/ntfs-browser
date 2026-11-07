// list files and irectories
// usage: ntfsdir "path"
// eg. ntfsdir c:\windows
// eg. ntfsdir "c:\program files\common files"
// eg. ntfsdir /dev/sda1/Windows
// eg. ntfsdir ./disk.img/Windows

#include <ntfs-browser/example/ntfsdir/ntfsdir-core.h>
#include <ntfs-browser/namespace.h>

namespace ExampleNtfsDir = NTFS_BROWSER_NAMESPACE::Example::NtfsDir;
namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

#ifdef _WIN32

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return ExampleNtfsDir::RunNtfsDir<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>(argc, argv);
}

#else

  #include <string>
  #include <vector>

  #include <ntfs-browser/tool/device-access/argv-wide.h>

int main(int argc, char* argv[]) {
  std::vector<std::wstring> wide =
      ToolDeviceAccess::ArgumentsAsWide(argc, argv);
  std::vector<wchar_t*> pointers = ToolDeviceAccess::ArgumentPointers(wide);
  return ExampleNtfsDir::RunNtfsDir<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>(argc,
                                                          pointers.data());
}

#endif
