// list files and irectories
// usage: ntfsdir "path"
// eg. ntfsdir c:\windows
// eg. ntfsdir "c:\program files\common files"
// eg. ntfsdir /dev/sda1/Windows
// eg. ntfsdir ./disk.img/Windows

#include <ntfs-browser/namespace.h>

#include "ntfsdir-core.h"

#ifdef _WIN32

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return RunNtfsDir<NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>(argc,
                                                                        argv);
}

#else

  #include <string>
  #include <vector>

  #include "argv-wide.h"

int main(int argc, char* argv[]) {
  std::vector<std::wstring> wide = NtfsDevice::ArgumentsAsWide(argc, argv);
  std::vector<wchar_t*> pointers = NtfsDevice::ArgumentPointers(wide);
  return RunNtfsDir<NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>(
      argc, pointers.data());
}

#endif
