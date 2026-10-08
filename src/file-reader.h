#pragma once

#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
  #include <string_view>
#endif

#include <ntfs-browser/strategy.h>

#include "internal-export.h"

namespace NtfsBrowser {

class IDiskReader;

template <Strategy S>
class NTFS_BROWSER_EXPORT_TESTS_ONLY FileReader {
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
  std::enable_if_t<
      std::is_same_v<std::integral_constant<Strategy, S2>,
                     std::integral_constant<Strategy, Strategy::NoCache>>,
      std::optional<std::span<const BYTE>>>
      Read(LARGE_INTEGER& addr, DWORD length) const;

  template <Strategy S2 = S>
  std::enable_if_t<
      std::is_same_v<std::integral_constant<Strategy, S2>,
                     std::integral_constant<Strategy, Strategy::FullCache>>,
      std::optional<std::span<const BYTE>>>
      Read(LARGE_INTEGER& addr, DWORD length) const;

 private:
  BYTE* NextMemory() const;

  BYTE* GetCachedBlock(LARGE_INTEGER block_addr) const;

  std::optional<std::span<const BYTE>> ReadUncached(LARGE_INTEGER addr,
                                                    DWORD length) const;

  std::unique_ptr<IDiskReader> reader_;

  // Use only for Strategy::NoCache.
  mutable std::vector<BYTE> buffer_;

  // Strategy::FullCache
  mutable std::unordered_map<size_t, BYTE*> map_buffer_;
  mutable std::vector<std::vector<BYTE>> mem_alloc_;
  mutable size_t last_alloc_ = 0;

  // Owns stitched-together buffers for crossing reads, kept alive for
  // this reader's lifetime.
  mutable std::vector<std::vector<BYTE>> crossing_reads_;
};

}  // namespace NtfsBrowser
