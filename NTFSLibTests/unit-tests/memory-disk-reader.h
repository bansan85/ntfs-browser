#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include <windows.h>

#include <ntfs-browser/disk-reader.h>

namespace NtfsBrowserTests
{

// A fake IDiskReader backed entirely by an in-memory buffer.
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
