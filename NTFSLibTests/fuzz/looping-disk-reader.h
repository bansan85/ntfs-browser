#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/win-types.h>

namespace NtfsFuzz
{

// A fake IDiskReader backed by a whole file in memory (the AFL testcase).
// ReadInto() wraps back to the start of the buffer past the end, instead
// of failing, so parsing can go arbitrarily deep off a small input.
class LoopingDiskReader : public NtfsBrowser::IDiskReader
{
 public:
  // Empty on a zero-length or unreadable file.
  [[nodiscard]] static std::optional<std::vector<BYTE>>
      LoadFile(const std::filesystem::path& path);

  explicit LoopingDiskReader(std::vector<BYTE> data);

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  std::vector<BYTE> data_;
  mutable size_t pos_{0};
};

}
