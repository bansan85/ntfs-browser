#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include <ntfs-browser/namespace.h>

#ifdef _WIN32
  #include <string_view>
#endif

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE {

class IDiskReader;

namespace Cache {

class BlockCache;

}  // namespace Cache

namespace Io {

template <Cache::Strategy S>
class NTFS_BROWSER_EXPORT_TESTS_ONLY FileReader {
 public:
  // max_cache_bytes is the soft ceiling of the FullCache block cache. NoCache
  // ignores it.
  explicit FileReader(
      std::uint64_t max_cache_bytes = Cache::default_max_cache_bytes);

  // Takes ownership of an already-open reader (eg. an in-memory test double)
  // instead of opening a real disk/file via Open().
  explicit FileReader(
      std::unique_ptr<IDiskReader> reader,
      std::uint64_t max_cache_bytes = Cache::default_max_cache_bytes);

#ifdef _WIN32
  // Opens a real disk/file path via Win32DiskReader. Not available outside
  // Windows -- construct FileReader from an already-open IDiskReader there.
  bool Open(std::wstring_view volume);
#endif

  // Wraps the reader in an Io::ReadAheadDiskReader with chunks of this many
  // bytes. Call it once the reader is open and before the first read.
  void EnableReadAhead(std::uint64_t chunk_bytes);

  // Reads from addr into dest.
  bool ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const;

  // Size of the reader's backing store, or nullopt when it has none or no
  // reader is set. Reads nothing.
  [[nodiscard]] std::optional<ULONGLONG> Size() const;

  // Announces to the reader that the bytes [offset, offset + length) are about
  // to be read. FullCache announces the whole blocks it will read instead,
  // and none that its cache holds.
  void Prefetch(ULONGLONG offset, ULONGLONG length) const;

  // Forgets what Prefetch() announced and no read used yet.
  void CancelPrefetch() const;

  // NoCache: a span over the reader's own buffer, valid until the next read.
  template <Cache::Strategy S2 = S>
  std::enable_if_t<
      std::is_same_v<
          std::integral_constant<Cache::Strategy, S2>,
          std::integral_constant<Cache::Strategy, Cache::Strategy::NoCache>>,
      std::optional<std::span<const BYTE>>>
      Read(LARGE_INTEGER& addr, DWORD length) const;

  // FullCache: a view that keeps the bytes valid while a copy of it lives.
  template <Cache::Strategy S2 = S>
  std::enable_if_t<
      std::is_same_v<
          std::integral_constant<Cache::Strategy, S2>,
          std::integral_constant<Cache::Strategy, Cache::Strategy::FullCache>>,
      std::optional<Cache::View>>
      Read(LARGE_INTEGER& addr, DWORD length) const;

  // Frees every cached block that no view uses. A no-op under NoCache.
  void Minify() const noexcept;

  // The memory the block cache holds. All zero under NoCache.
  [[nodiscard]] Cache::Stats GetCacheStats() const noexcept;

 private:
  [[nodiscard]] std::optional<Cache::View> ReadUncached(LARGE_INTEGER addr,
                                                        DWORD length) const;

  std::unique_ptr<IDiskReader> reader_;

  // Use only for Cache::Strategy::NoCache.
  mutable std::vector<BYTE> buffer_;

  // Cache::Strategy::FullCache. Null under NoCache. Shared, since the views
  // reach it through a weak pointer.
  std::shared_ptr<Cache::BlockCache> cache_;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
