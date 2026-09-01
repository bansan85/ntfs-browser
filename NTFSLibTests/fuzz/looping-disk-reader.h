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

// A disk reader backed by a whole file loaded into memory (the AFL
// testcase). Every ReadInto() serves the next dest.size() bytes, wrapping
// back to the start of the buffer whenever it runs past the end instead of
// signalling EOF -- this lets NtfsVolume/FileRecord walk arbitrarily deep
// off a small, AFL-mutated input, the same way the RNG-backed clang fuzzer
// never runs dry.
class LoopingDiskReader : public NtfsBrowser::IDiskReader
{
 public:
  // Empty on a zero-length or unreadable file -- callers should treat that
  // as "nothing to fuzz this run" rather than constructing a reader that
  // could never make progress.
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

}  // namespace NtfsFuzz
