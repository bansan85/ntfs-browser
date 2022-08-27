#pragma once

#include <memory>
#include <string_view>

#include <windows.h>

namespace NtfsBrowser
{

class FileReader
{
 public:
  FileReader();

  bool Open(std::wstring_view volume);
  bool Read(LARGE_INTEGER& addr, DWORD length, void* buf) const;

 private:
  using HandlePtr = std::unique_ptr<std::remove_pointer<HANDLE>::type,
                                    decltype(&::CloseHandle)>;

  HandlePtr handle_;
};

}  // namespace NtfsBrowser