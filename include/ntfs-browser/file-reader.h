#pragma once

#include <ntfs-browser/win-types.h>

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/export.h>
#include <ntfs-browser/strategy.h>

namespace NtfsBrowser
{

template <Strategy S>
class NTFS_BROWSER_EXPORT FileReader
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

  // Reads from addr into dest.
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

  BYTE* GetCachedBlock(LARGE_INTEGER blockAddr) const;

  std::unique_ptr<IDiskReader> reader_;

  // Use only for Strategy::NO_CACHE.
  mutable std::vector<BYTE> buffer_;

  // Strategy::FULL_CACHE
  mutable std::unordered_map<size_t, BYTE*> map_buffer_;
  mutable std::vector<std::unique_ptr<BYTE[]>> mem_alloc;
  mutable size_t last_alloc = 0;

  // Owns stitched-together buffers for crossing reads, kept alive for
  // this reader's lifetime.
  mutable std::vector<std::unique_ptr<BYTE[]>> crossing_reads_;
};

}  // namespace NtfsBrowser
