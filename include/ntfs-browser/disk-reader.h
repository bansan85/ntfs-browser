#pragma once

#include <span>
#include <string_view>

#include <ntfs-browser/win-types.h>

namespace NtfsBrowser
{

// Abstracts "get raw bytes from a backing store" so FileReader<S> doesn't
// depend on a Win32 file/device handle directly. Implementations decide what
// addr/dest actually mean (eg. an absolute disk offset, or an ignored
// parameter for a sequential fake).
class IDiskReader
{
 public:
  virtual ~IDiskReader() = default;

  virtual bool Open(std::wstring_view path) = 0;

  [[nodiscard]] virtual bool ReadInto(LARGE_INTEGER& addr,
                                      std::span<BYTE> dest) const = 0;
};

}  // namespace NtfsBrowser
