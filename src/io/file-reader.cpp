#include "io/file-reader.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/read-ahead-disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "cache/block-cache.h"
#include "cache/view-key.h"
#include "internal-export.h"
#include "log/ntfs-common.h"

#ifdef _WIN32
  #include <string_view>

  #include "io/win32-disk-reader.h"
#endif

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// Size of one block read from the disk and cached. 64 KiB amortises the cost
// of a disk read without holding much more than a cluster run's worth of data.
constexpr LONGLONG read_buffer_size = LONGLONG{64} * 1024;

// read_buffer_size as a size_t, to size a std::span over one cached block.
constexpr size_t block_bytes_value = static_cast<size_t>(read_buffer_size);

// A BlockCache for FullCache only: NoCache keeps none.
template <Cache::Strategy S>
std::shared_ptr<Cache::BlockCache> MakeCache(std::uint64_t max_cache_bytes) {
  if constexpr (S == Cache::Strategy::FullCache) {
    return std::make_shared<Cache::BlockCache>(block_bytes_value,
                                               max_cache_bytes);
  } else {
    return nullptr;
  }
}

}  // namespace

template <Cache::Strategy S>
FileReader<S>::FileReader(std::uint64_t max_cache_bytes)
    : cache_(MakeCache<S>(max_cache_bytes)) {}

template <Cache::Strategy S>
FileReader<S>::FileReader(std::unique_ptr<IDiskReader> reader,
                          std::uint64_t max_cache_bytes)
    : reader_(std::move(reader)), cache_(MakeCache<S>(max_cache_bytes)) {}

#ifdef _WIN32
template <Cache::Strategy S>
bool FileReader<S>::Open(std::wstring_view volume) {
  auto reader = std::make_unique<Win32DiskReader>();
  if (!reader->Open(volume)) {
    return false;
  }

  reader_ = std::move(reader);
  return true;
}
#endif

template <Cache::Strategy S>
void FileReader<S>::EnableReadAhead(std::uint64_t chunk_bytes) {
  if (reader_) {
    reader_ = std::make_unique<ReadAheadDiskReader>(
        std::move(reader_), ReadAheadOptions{.chunk_bytes = chunk_bytes});
  }
}

template <Cache::Strategy S>
bool FileReader<S>::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const {
  return reader_->ReadInto(addr, dest);
}

template <Cache::Strategy S>
std::optional<ULONGLONG> FileReader<S>::Size() const {
  return reader_ ? reader_->Size() : std::nullopt;
}

template <Cache::Strategy S>
void FileReader<S>::Prefetch(ULONGLONG offset, ULONGLONG length) const {
  if (!reader_ || length == 0) {
    return;
  }
  if constexpr (S == Cache::Strategy::FullCache) {
    constexpr auto block = static_cast<ULONGLONG>(read_buffer_size);
    // The blocks MUST end below the largest address the cache reads at.
    constexpr auto max_end =
        static_cast<ULONGLONG>(std::numeric_limits<LONGLONG>::max()) - block;
    if (offset > max_end || length > max_end - offset) {
      return;
    }
    const ULONGLONG first = offset - offset % block;
    const ULONGLONG last = offset + length + block - 1;
    const ULONGLONG end = last - last % block;
    // A range of more blocks than this is announced whole, so a forged length
    // costs no walk over the cache.
    constexpr ULONGLONG max_checked_blocks = 4096;
    if ((end - first) / block > max_checked_blocks) {
      reader_->Prefetch(first, end - first);
      return;
    }
    // The cache serves a block it holds without a read, so only the other
    // blocks are announced: a range nothing reads would be read for nothing.
    std::optional<ULONGLONG> run_start;
    for (ULONGLONG at = first; at < end; at += block) {
      const bool cached = cache_->Contains(at / block);
      if (cached && run_start) {
        reader_->Prefetch(*run_start, at - *run_start);
        run_start.reset();
      } else if (!cached && !run_start) {
        run_start = at;
      }
    }
    if (run_start) {
      reader_->Prefetch(*run_start, end - *run_start);
    }
  } else {
    reader_->Prefetch(offset, length);
  }
}

template <Cache::Strategy S>
void FileReader<S>::CancelPrefetch() const {
  if (reader_) {
    reader_->CancelPrefetch();
  }
}

template <Cache::Strategy T>
template <Cache::Strategy Q>
std::enable_if_t<std::is_same_v<std::integral_constant<Cache::Strategy, Q>,
                                std::integral_constant<
                                    Cache::Strategy, Cache::Strategy::NoCache>>,
                 std::optional<std::span<const BYTE>>>
    FileReader<T>::Read(LARGE_INTEGER& addr, DWORD length) const {
  if (buffer_.size() < length) {
    buffer_.resize(length);
  }

  if (!reader_->ReadInto(addr, std::span<BYTE>{buffer_.data(), length})) {
    Log::Error("Cannot read file at adress {}", addr.QuadPart);
    return {};
  }

  return std::span<const BYTE>{buffer_.data(), length};
}

// Reads exactly "length" bytes at "addr" into a buffer that only the returned
// view owns, bypassing the block cache. FullCache falls back to it when a
// whole 64KiB block cannot be read: the block may extend past the end of the
// medium. The short block MUST NOT be cached as if it were complete.
template <Cache::Strategy S>
std::optional<Cache::View> FileReader<S>::ReadUncached(LARGE_INTEGER addr,
                                                       DWORD length) const {
  const Cache::BlockCache::Stitched exact = cache_->NewStitched(length);
  if (!reader_->ReadInto(addr, exact.bytes)) {
    Log::Error("Cannot read file at adress {}", addr.QuadPart);
    return {};
  }

  Cache::View view{Cache::ViewKey{}, exact.bytes, exact.owner, cache_};
  cache_->Trim();
  return view;
}

template <Cache::Strategy T>
template <Cache::Strategy Q>
std::enable_if_t<
    std::is_same_v<
        std::integral_constant<Cache::Strategy, Q>,
        std::integral_constant<Cache::Strategy, Cache::Strategy::FullCache>>,
    std::optional<Cache::View>>
    FileReader<T>::Read(LARGE_INTEGER& addr, DWORD length) const {
  if (length == 0) {
    return Cache::View{};
  }

  // A negative address has no block to cache. An end past LLONG_MAX would
  // overflow the block arithmetic below.
  if (addr.QuadPart < 0 ||
      addr.QuadPart > std::numeric_limits<LONGLONG>::max() - length) {
    Log::Error("Cannot read file at adress {}: range is out of bounds",
               addr.QuadPart);
    return {};
  }

  // Loads the block at block_addr, or returns the cached one. Null when the
  // disk read fails or comes back short: nothing is cached then.
  const auto get_block =
      [this](LARGE_INTEGER block_addr) -> std::shared_ptr<Cache::Block> {
    return cache_->GetOrLoad(
        static_cast<std::uint64_t>(block_addr.QuadPart / read_buffer_size),
        [&](std::span<BYTE> dest) {
          return reader_->ReadInto(block_addr, dest);
        });
  };

  const bool crosses_block = addr.QuadPart / read_buffer_size !=
                             (addr.QuadPart + length - 1) / read_buffer_size;

  if (!crosses_block) {
    // Fast path: the request fits in a single block; return a zero-copy view.
    const LARGE_INTEGER block_addr{
        .QuadPart = addr.QuadPart - addr.QuadPart % read_buffer_size};
    const std::shared_ptr<Cache::Block> block = get_block(block_addr);
    if (!block) {
      return ReadUncached(addr, length);
    }

    const std::span<const BYTE> bytes =
        std::span<const BYTE>{block->bytes.get(), block_bytes_value}.subspan(
            gsl::narrow<size_t>(addr.QuadPart % read_buffer_size), length);
    Cache::View view{Cache::ViewKey{}, bytes, block, cache_};
    cache_->Trim();
    return view;
  }

  // Slow path: stitch the range together one block at a time. The buffer
  // belongs to the view alone, so the blocks stay free to be evicted.
  const Cache::BlockCache::Stitched stitched = cache_->NewStitched(length);
  std::span<BYTE> out = stitched.bytes;

  LARGE_INTEGER cur = addr;
  DWORD remaining = length;
  while (remaining != 0) {
    const LARGE_INTEGER block_addr{.QuadPart = cur.QuadPart -
                                               cur.QuadPart % read_buffer_size};
    const std::shared_ptr<Cache::Block> block = get_block(block_addr);
    if (!block) {
      return ReadUncached(addr, length);
    }

    const auto offset_in_block =
        gsl::narrow<DWORD>(cur.QuadPart % read_buffer_size);
    const auto chunk = gsl::narrow<DWORD>(
        std::min<LONGLONG>(read_buffer_size - offset_in_block, remaining));

    const std::span<const BYTE> block_bytes{block->bytes.get(),
                                            block_bytes_value};
    // offsetInBlock is a remainder modulo READ_BUFFER_SIZE = block_bytes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    memcpy(out.data(), &block_bytes[offset_in_block], chunk);

    out = out.subspan(chunk);
    cur.QuadPart += chunk;
    remaining -= chunk;
    // The block is copied: it is free to go if the read outgrows the ceiling.
    cache_->Trim();
  }

  Cache::View view{Cache::ViewKey{}, stitched.bytes, stitched.owner, cache_};
  cache_->Trim();
  return view;
}

template <Cache::Strategy S>
void FileReader<S>::Minify() const noexcept {
  if (cache_) {
    cache_->Minify();
  }
}

template <Cache::Strategy S>
Cache::Stats FileReader<S>::GetCacheStats() const noexcept {
  return cache_ ? cache_->GetStats() : Cache::Stats{};
}

template class FileReader<Cache::Strategy::NoCache>;
template class FileReader<Cache::Strategy::FullCache>;

// Class-level NTFS_BROWSER_EXPORT_TESTS_ONLY (on FileReader) does not reach a
// member function template's own explicit instantiations: each needs the
// macro again here, or the unit tests cannot link against it on a shared
// build.
template NTFS_BROWSER_EXPORT_TESTS_ONLY std::optional<std::span<const BYTE>>
    FileReader<Cache::Strategy::NoCache>::Read<Cache::Strategy::NoCache>(
        LARGE_INTEGER& addr, DWORD length) const;
template NTFS_BROWSER_EXPORT_TESTS_ONLY std::optional<Cache::View>
    FileReader<Cache::Strategy::FullCache>::Read<Cache::Strategy::FullCache>(
        LARGE_INTEGER& addr, DWORD length) const;

}  // namespace NTFS_BROWSER_NAMESPACE::Io
