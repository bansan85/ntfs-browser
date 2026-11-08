// Lists the files of a folder or of a whole NTFS volume with Search::Scan.
// usage: ntfsdir [options] "path"
// eg. ntfsdir c:\windows
// eg. ntfsdir -r "c:\program files\common files" --name=*.dll
// eg. ntfsdir /dev/sda1/Windows
// eg. ntfsdir ./disk.img/Windows --engine=mftlist

#include <ntfs-browser/example/ntfsdir/ntfsdir-core.h>
#include <ntfs-browser/namespace.h>

namespace ExampleNtfsDir = NTFS_BROWSER_NAMESPACE::Example::NtfsDir;

#ifdef _WIN32

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return ExampleNtfsDir::RunNtfsDir(argc, argv);
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
  return ExampleNtfsDir::RunNtfsDir(argc, pointers.data());
}

#endif
