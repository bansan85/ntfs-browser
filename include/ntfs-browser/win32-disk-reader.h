#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include <ntfs-browser/disk-reader.h>

#include <windows.h>

namespace NtfsBrowser
{

// Production IDiskReader: a real disk/device, or an ordinary file treated
// the same way (CreateFileW handles both identically).
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

}
