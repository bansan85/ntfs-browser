#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <span>
#include <vector>
#include <unordered_map>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/strategy.h>

#include <windows.h>

namespace NtfsBrowser
{

template <Strategy S>
class FileReader
{
 public:
  FileReader();

  // Takes ownership of an already-open reader (eg. an in-memory test double)
  // instead of opening a real disk/file via Open().
  explicit FileReader(std::unique_ptr<IDiskReader> reader);

  bool Open(std::wstring_view volume);

  // Reads directly into a caller-owned destination, bypassing buffer_/
  // map_buffer_ entirely. Use when the result must outlive the next Read().
  bool ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const;

  template <Strategy S2 = S>
  typename std::enable_if_t<
      std::is_same_v<std::integral_constant<Strategy, S2>,
                     std::integral_constant<Strategy, Strategy::NO_CACHE>>,
      std::optional<std::span<const BYTE>>>
      Read(LARGE_INTEGER& addr, DWORD length) const;

  template <Strategy S2 = S>
  typename std::enable_if_t<
      std::is_same_v<std::integral_constant<Strategy, S2>,
                     std::integral_constant<Strategy, Strategy::FULL_CACHE>>,
      std::optional<std::span<const BYTE>>>
      Read(LARGE_INTEGER& addr, DWORD length) const;

 private:
  BYTE* NextMemory() const;

  std::unique_ptr<IDiskReader> reader_;

  // Use only for Strategy::NO_CACHE.
  mutable std::vector<BYTE> buffer_;

  // Strategy::FULL_CACHE
  mutable std::unordered_map<size_t, BYTE*> map_buffer_;
  mutable std::vector<std::unique_ptr<BYTE[]>> mem_alloc;
  mutable size_t last_alloc = 0;
};

}  // namespace NtfsBrowser
