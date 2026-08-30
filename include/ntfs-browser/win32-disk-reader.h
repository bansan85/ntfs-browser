#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include <ntfs-browser/disk-reader.h>

#include <windows.h>

namespace NtfsBrowser
{

// Real disk/device reader, also usable to read an ordinary file as if it
// were a disk (CreateFileW treats both the same way): the production
// implementation of IDiskReader, and the one used to read a disk image file
// in tests.
class Win32DiskReader : public IDiskReader
{
 public:
  Win32DiskReader();

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  using HandlePtr =
      std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>;

  HandlePtr handle_;
};

}  // namespace NtfsBrowser
