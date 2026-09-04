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
#include <ntfs-browser/win-types.h>

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

#ifdef _WIN32
  // Opens a real disk/file path via Win32DiskReader. Not available outside
  // Windows -- construct FileReader from an already-open IDiskReader there.
  bool Open(std::wstring_view volume);
#endif

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

  // Strategy::FULL_CACHE only. See file-reader.cpp for details.
  BYTE* GetCachedBlock(LARGE_INTEGER blockAddr) const;

  std::unique_ptr<IDiskReader> reader_;

  // Use only for Strategy::NO_CACHE.
  mutable std::vector<BYTE> buffer_;

  // Strategy::FULL_CACHE
  mutable std::unordered_map<size_t, BYTE*> map_buffer_;
  mutable std::vector<std::unique_ptr<BYTE[]>> mem_alloc;
  mutable size_t last_alloc = 0;

  // Strategy::FULL_CACHE only, and only ever appended to when a single
  // Read() call spans more than one of NextMemory()'s 64KiB blocks (see
  // Read<FULL_CACHE>() in file-reader.cpp for why that can't be served as a
  // zero-copy view like the common, single-block case). Owns the resulting
  // stitched-together copies for as long as this FileReader lives, so a
  // span returned for a crossing read stays valid exactly as long as one
  // returned for a non-crossing read would (FULL_CACHE's whole point).
  mutable std::vector<std::unique_ptr<BYTE[]>> crossing_reads_;
};

}  // namespace NtfsBrowser
