// usage: ntfsundel [options] "target"
// eg. ntfsundel ./disk.img --output /mnt/backup/out --all
// eg. ntfsundel /dev/sda1 --output /mnt/backup/out --name "*.docx"
// eg. ntfsundel c: --output d:\out --record 1234

#include <ntfs-browser/example/ntfsundel/ntfsundel-cli.h>
#include <ntfs-browser/namespace.h>

namespace ExampleNtfsUndel = NTFS_BROWSER_NAMESPACE::Example::NtfsUndel;

#ifdef _WIN32

// NOLINTNEXTLINE(readability-identifier-naming): wmain is the CRT entry point.
int wmain(int argc, wchar_t* argv[]) {
  return ExampleNtfsUndel::RunNtfsUndel(argc, argv);
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
  return ExampleNtfsUndel::RunNtfsUndel(argc, pointers.data());
}

#endif
