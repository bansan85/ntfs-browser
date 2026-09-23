#pragma once

#include <ntfs-browser/win-types.h>

#include <span>
#include <string_view>

namespace NtfsBrowser
{

// Abstracts "get raw bytes from a backing store" so FileReader<S> doesn't
// depend on a Win32 file/device handle directly. An implementation decides
// what addr/dest mean (eg. a disk offset, or an ignored parameter).
class IDiskReader
{
 public:
  virtual ~IDiskReader() = default;

  // Opens the backing store at path.
  virtual bool Open(std::wstring_view path) = 0;

  // Reads from addr into dest.
  [[nodiscard]] virtual bool ReadInto(LARGE_INTEGER& addr,
                                      std::span<BYTE> dest) const = 0;
};

}  // namespace NtfsBrowser
