// usage: ntfsattr [options] "target"
// eg. ntfsattr ./disk.img/Windows/notepad.exe
// eg. ntfsattr /dev/sda1/Windows/notepad.exe
// eg. ntfsattr c:\windows\notepad.exe

#include <ntfs-browser/example/ntfsattr/ntfsattr-core.h>
#include <ntfs-browser/namespace.h>

namespace ExampleNtfsAttr = NTFS_BROWSER_NAMESPACE::Example::NtfsAttr;

#ifdef _WIN32

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return ExampleNtfsAttr::RunNtfsAttr(argc, argv);
}

#else

  #include <string>
  #include <vector>

  #include <ntfs-browser/tool/device-access/argv-wide.h>

int main(int argc, char* argv[]) {
  std::vector<std::wstring> wide =
      NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess::ArgumentsAsWide(argc, argv);
  std::vector<wchar_t*> pointers =
      NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess::ArgumentPointers(wide);
  return ExampleNtfsAttr::RunNtfsAttr(argc, pointers.data());
}

#endif
