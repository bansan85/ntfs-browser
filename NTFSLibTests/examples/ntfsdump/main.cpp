// usage: ntfsdump [options] "target"
// eg. ntfsdump ./disk.img/Windows/notepad.exe
// eg. ntfsdump /dev/sda1/Windows/notepad.exe
// eg. ntfsdump c:\windows\notepad.exe

#include <ntfs-browser/example/ntfsdump/ntfsdump-core.h>
#include <ntfs-browser/namespace.h>

namespace ExampleNtfsDump = NTFS_BROWSER_NAMESPACE::Example::NtfsDump;

#ifdef _WIN32

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return ExampleNtfsDump::RunNtfsDump(argc, argv);
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
  return ExampleNtfsDump::RunNtfsDump(argc, pointers.data());
}

#endif
