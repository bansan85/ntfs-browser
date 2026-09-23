#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <ntfs-browser/disk-reader.h>

namespace NtfsFuzz
{

// A fake IDiskReader backed by a whole file in memory (the AFL testcase).
// ReadInto() wraps back to the start of the buffer past the end, instead
// of failing, so parsing can go arbitrarily deep off a small input.
// It borrows the buffer, which MUST outlive the reader.
class LoopingDiskReader : public NtfsBrowser::IDiskReader
{
 public:
  // Empty on a zero-length or unreadable file.
  [[nodiscard]] static std::optional<std::vector<BYTE>>
      LoadFile(const std::filesystem::path& path);

  // failingRead is the 0-based index of the one ReadInto() call that fails.
  explicit LoopingDiskReader(std::span<const BYTE> data,
                             std::optional<size_t> failingRead = {});

  bool Open(std::wstring_view path) override;

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override;

 private:
  std::span<const BYTE> data_;
  std::optional<size_t> failing_read_;
  mutable size_t reads_{0};
  mutable size_t pos_{0};
};

}  // namespace NtfsFuzz
