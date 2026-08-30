#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>

#include <windows.h>

namespace NtfsBrowserTests
{

// Same random-access-by-offset semantics as
// NtfsBrowser::Win32DiskReader, but served entirely out of an in-memory
// buffer instead of a real file/device handle - avoids disk I/O in tests.
class MemoryDiskReader : public NtfsBrowser::IDiskReader
{
 public:
  // data is the whole fake volume/file content, ready to be read from.
  explicit MemoryDiskReader(std::vector<BYTE> data);

  // Loads a real file's content once, so subsequent reads never touch disk.
  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  std::vector<BYTE> data_;
};

}  // namespace NtfsBrowserTests
