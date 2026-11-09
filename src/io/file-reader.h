#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "tuning/tuning.h"
#include "win-types.h"

#ifdef _WIN32
  #include <string_view>
#endif

#include "cache/stats.h"
#include "cache/strategy.h"
#include "cache/view.h"
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
  // ignores it. tuning sets the block of the cache and, for Open(), the
  // read-ahead caps of the reader it opens.
  explicit FileReader(
      std::uint64_t max_cache_bytes = Cache::default_max_cache_bytes,
      const Tuning::Settings& tuning = {});

  // Takes ownership of an already-open reader (eg. an in-memory test double)
  // instead of opening a real disk/file via Open().
  explicit FileReader(
      std::unique_ptr<IDiskReader> reader,
      std::uint64_t max_cache_bytes = Cache::default_max_cache_bytes,
      const Tuning::Settings& tuning = {});

#ifdef _WIN32
  // Opens a real disk/file path via Win32DiskReader. Not available outside
  // Windows -- construct FileReader from an already-open IDiskReader there.
  // ask_host_volume false keeps the reader from asking the volume that hosts
  // an image file for its media kind: a caller that states the kind itself
  // has no use for the answer.
  bool Open(std::wstring_view volume, bool ask_host_volume = true);
#endif

  // Wraps the reader in an Io::ReadAheadDiskReader with chunks of this many
  // bytes. Call it once the reader is open and before the first read.
  void EnableReadAhead(std::uint64_t chunk_bytes,
                       std::size_t window_chunks = 2);

  // Reads from addr into dest.
  bool ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const;

  // Size of the reader's backing store, or nullopt when it has none or no
  // reader is set. Reads nothing.
  [[nodiscard]] std::optional<ULONGLONG> Size() const;

  // Kind of device behind the reader, Unknown when no reader is set. Reads
  // nothing.
  [[nodiscard]] Tuning::MediaKind GetMediaKind() const;

  // Announces to the reader that the bytes [offset, offset + length) are about
  // to be read. FullCache announces the whole blocks it will read instead,
  // and none that its cache holds.
  void Prefetch(ULONGLONG offset, ULONGLONG length) const;

  // Forgets what Prefetch() announced and no read used yet.
  void CancelPrefetch() const;

  // Like Prefetch(), on the reader's standing lane: the same widening to whole
  // blocks under FullCache, and the same blocks left out. A read of a run of
  // missing blocks is served when one announced range holds the whole run.
  void PrefetchStanding(ULONGLONG offset, ULONGLONG length) const;

  // Withdraws the standing ranges that overlap [offset, offset + length).
  // FullCache widens the interval to whole blocks first, as PrefetchStanding()
  // does.
  void CancelStanding(ULONGLONG offset, ULONGLONG length) const;

  // Tells the reader which read pattern the reads that follow belong to.
  void SetReadPattern(Tuning::Pattern pattern) const;

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

  // Every read the volume makes goes through here, so that a benchmark session
  // times it and a capture sink sees its bytes. Without either it is a plain
  // call of the reader.
  [[nodiscard]] bool ReadFromReader(LARGE_INTEGER& addr,
                                    std::span<BYTE> dest) const;

  // Hands one range to the reader as a hint, and logs it for a session.
  void Announce(ULONGLONG offset, ULONGLONG length) const;

  // Hands one range to the reader's standing lane.
  void AnnounceStanding(ULONGLONG offset, ULONGLONG length) const;

  // The body of Prefetch() and PrefetchStanding(): cuts the range into the
  // runs of blocks to announce, on the standing lane when standing is set.
  void AnnounceRuns(ULONGLONG offset, ULONGLONG length, bool standing) const;

  // Announces one range on the first queue, or on the standing lane.
  void AnnounceOne(ULONGLONG offset, ULONGLONG length, bool standing) const;

  // Announces the runs of [first, end) that the cache lacks.
  void AnnounceUncachedBlocks(ULONGLONG first, ULONGLONG end,
                              bool standing) const;

  std::unique_ptr<IDiskReader> reader_;

  // Bytes of one cached block, the most one disk read covers when a read
  // spans several missing blocks, and the read-ahead caps of the reader
  // Open() makes. Fixed at construction.
  std::int64_t block_bytes_;
  std::int64_t max_run_bytes_;
  std::array<Tuning::ReaderTuning, Tuning::pattern_count> reader_caps_;

  // Use only for Cache::Strategy::NoCache.
  mutable std::vector<BYTE> buffer_;

  // Cache::Strategy::FullCache. Null under NoCache. Shared, since the views
  // reach it through a weak pointer.
  std::shared_ptr<Cache::BlockCache> cache_;
};

}  // namespace Io

}  // namespace NTFS_BROWSER_NAMESPACE
