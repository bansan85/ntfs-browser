#include "io/file-reader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "bench/recorder.h"
#include "cache/block-cache.h"
#include "cache/stats.h"
#include "cache/strategy.h"
#include "cache/view-key.h"
#include "cache/view.h"
#include "disk-reader.h"
#include "internal-export.h"
#include "io/read-ahead-disk-reader.h"
#include "log/ntfs-common.h"
#include "tuning/tuning.h"
#include "win-types.h"

#ifdef _WIN32
  #include <string_view>

  #include "io/win32-disk-reader.h"
#endif

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// A block of a cache is at least one sector: a smaller one would be a read
// the device refuses, and a block of 0 would divide by zero.
constexpr std::size_t min_block_bytes = 4096;

// Bytes of one block of the cache, from the tuning: Tuning::CacheTuning gives
// the default, 4 KiB, the size of an index block.
std::int64_t BlockBytesOf(const Tuning::Settings& tuning) {
  return static_cast<std::int64_t>(
      (std::max)(tuning.cache.block_bytes, min_block_bytes));
}

// Bytes one disk read covers at most when a read spans missing blocks: the
// tuning, and never under one block.
std::int64_t MaxRunBytesOf(const Tuning::Settings& tuning,
                           std::int64_t block_bytes) {
  const std::size_t wanted =
      (std::min)(tuning.cache.max_run_bytes,
                 static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
  return (std::max)(static_cast<std::int64_t>(wanted), block_bytes);
}

// A BlockCache for FullCache only: NoCache keeps none.
template <Cache::Strategy S>
std::shared_ptr<Cache::BlockCache> MakeCache(std::uint64_t max_cache_bytes,
                                             std::int64_t block_bytes) {
  if constexpr (S == Cache::Strategy::FullCache) {
    return std::make_shared<Cache::BlockCache>(static_cast<size_t>(block_bytes),
                                               max_cache_bytes);
  } else {
    static_cast<void>(block_bytes);
    return nullptr;
  }
}

// Loads the blocks of one FullCache read. A block that the cache lacks comes
// in with the missing blocks after it, up to the end of the range, in one disk
// read. The run is cut into blocks as the copy reaches them, so a small
// ceiling never evicts a block before its use.
template <typename ReadFn>
class RunBlockLoader {
 public:
  RunBlockLoader(Cache::BlockCache& cache, std::int64_t block_bytes,
                 std::int64_t max_run_bytes, std::uint64_t last_index,
                 ReadFn read)
      : cache_(cache),
        block_bytes_(block_bytes),
        max_run_blocks_(
            static_cast<std::uint64_t>(max_run_bytes / block_bytes)),
        last_index_(last_index),
        read_(read) {}

  // Loads the block at block_addr, or returns the cached one. Null when the
  // disk read fails or comes back short: nothing is cached then.
  std::shared_ptr<Cache::Block> Get(LARGE_INTEGER block_addr) {
    const auto index =
        static_cast<std::uint64_t>(block_addr.QuadPart / block_bytes_);
    if (std::shared_ptr<Cache::Block> hit = cache_.Find(index)) {
      return hit;
    }
    const bool in_run = run_count_ != 0 && index >= run_first_ &&
                        index - run_first_ < run_count_;
    if (!in_run && !ReadRun(index, block_addr)) {
      return cache_.GetOrLoad(
          index, [&](std::span<BYTE> dest) { return read_(block_addr, dest); });
    }
    auto block =
        std::make_shared<Cache::Block>(static_cast<size_t>(block_bytes_));
    const std::span<const BYTE> source{
        run_bytes_.get(), static_cast<size_t>(run_count_ * block_bytes_)};
    const std::span<const BYTE> part =
        source.subspan(static_cast<size_t>((index - run_first_) * block_bytes_),
                       static_cast<size_t>(block_bytes_));
    std::ranges::copy(part, block->bytes.get());
    cache_.Insert(index, block);
    if (index - run_first_ + 1 == run_count_) {
      run_count_ = 0;
    }
    return block;
  }

 private:
  // Reads the missing blocks from index on with one disk read, at most
  // max_run_blocks_ of them and none past the range. False when there is only
  // one, or when the read failed: the block is then read on its own.
  bool ReadRun(std::uint64_t index, LARGE_INTEGER block_addr) {
    std::uint64_t count = 1;
    while (count < max_run_blocks_ && index + count <= last_index_ &&
           !cache_.Contains(index + count)) {
      ++count;
    }
    if (count == 1 || failed_) {
      return false;
    }
    const auto run_size = static_cast<size_t>(count * block_bytes_);
    if (run_capacity_ < run_size) {
      // Grown only when this run is longer than any before it in this read.
      run_capacity_ = run_size;
      run_bytes_ = std::make_unique_for_overwrite<BYTE[]>(run_size);
    }
    if (!read_(block_addr, {run_bytes_.get(), run_size})) {
      failed_ = true;
      return false;
    }
    run_first_ = index;
    run_count_ = count;
    return true;
  }

  Cache::BlockCache& cache_;
  std::int64_t block_bytes_;
  std::uint64_t max_run_blocks_;
  std::uint64_t last_index_;
  ReadFn read_;
  std::uint64_t run_first_{0};
  std::uint64_t run_count_{0};
  // Kept from one run to the next of the same read, so that its pages are
  // faulted in once.
  std::unique_ptr<BYTE[]> run_bytes_;
  size_t run_capacity_{0};
  bool failed_{false};
};

// Reads like reader.ReadInto(), and tells the bench recorder when the read
// began, ended and what it covered.
bool RecordedRead(IDiskReader& reader, LARGE_INTEGER& addr,
                  std::span<BYTE> dest) {
  Bench::Detail::ResetReadNote();
  const std::uint64_t began = Bench::Detail::NowNs();
  const bool read = reader.ReadInto(addr, dest);
  const std::uint64_t ended = Bench::Detail::NowNs();
  // A negative address is a refused read, and has no place on the disk.
  if (addr.QuadPart >= 0) {
    Bench::Detail::RecordRead(static_cast<std::uint64_t>(addr.QuadPart),
                              dest.size(), began, ended);
    if (read) {
      Bench::Detail::CaptureRead(static_cast<std::uint64_t>(addr.QuadPart),
                                 dest);
    }
  }
  return read;
}

// Copies the part of the range at cur that block holds to the front of out,
// then moves out, cur and remaining past it.
void CopyBlockPart(const Cache::Block& block, std::int64_t block_bytes,
                   std::span<BYTE>& out, LARGE_INTEGER& cur, DWORD& remaining) {
  const auto offset_in_block = gsl::narrow<DWORD>(cur.QuadPart % block_bytes);
  const auto chunk = gsl::narrow<DWORD>(
      std::min<LONGLONG>(block_bytes - offset_in_block, remaining));

  const std::span<const BYTE> bytes{block.bytes.get(),
                                    static_cast<size_t>(block_bytes)};
  // offset_in_block is a remainder modulo the block size = block_bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  memcpy(out.data(), &bytes[offset_in_block], chunk);

  out = out.subspan(chunk);
  cur.QuadPart += chunk;
  remaining -= chunk;
}

}  // namespace

template <Cache::Strategy S>
FileReader<S>::FileReader(std::uint64_t max_cache_bytes,
                          const Tuning::Settings& tuning)
    : block_bytes_(BlockBytesOf(tuning)),
      max_run_bytes_(MaxRunBytesOf(tuning, block_bytes_)),
      reader_caps_(tuning.reader),
      cache_(MakeCache<S>(max_cache_bytes, block_bytes_)) {}

template <Cache::Strategy S>
FileReader<S>::FileReader(std::unique_ptr<IDiskReader> reader,
                          std::uint64_t max_cache_bytes,
                          const Tuning::Settings& tuning)
    : reader_(std::move(reader)),
      block_bytes_(BlockBytesOf(tuning)),
      max_run_bytes_(MaxRunBytesOf(tuning, block_bytes_)),
      reader_caps_(tuning.reader),
      cache_(MakeCache<S>(max_cache_bytes, block_bytes_)) {}

#ifdef _WIN32
template <Cache::Strategy S>
bool FileReader<S>::Open(std::wstring_view volume, bool ask_host_volume) {
  auto reader = std::make_unique<Win32DiskReader>(reader_caps_);
  reader->SetAskHostVolume(ask_host_volume);
  if (!reader->Open(volume)) {
    return false;
  }

  reader_ = std::move(reader);
  return true;
}
#endif

template <Cache::Strategy S>
void FileReader<S>::EnableReadAhead(std::uint64_t chunk_bytes,
                                    std::size_t window_chunks) {
  if (reader_) {
    reader_ = std::make_unique<ReadAheadDiskReader>(
        std::move(reader_), ReadAheadOptions{.chunk_bytes = chunk_bytes,
                                             .window_chunks = window_chunks});
  }
}

template <Cache::Strategy S>
bool FileReader<S>::ReadInto(LARGE_INTEGER& addr, std::span<BYTE> dest) const {
  return ReadFromReader(addr, dest);
}

template <Cache::Strategy S>
bool FileReader<S>::ReadFromReader(LARGE_INTEGER& addr,
                                   std::span<BYTE> dest) const {
  if (!Bench::Detail::Active()) {
    return reader_->ReadInto(addr, dest);
  }
  return RecordedRead(*reader_, addr, dest);
}

template <Cache::Strategy S>
std::optional<ULONGLONG> FileReader<S>::Size() const {
  return reader_ ? reader_->Size() : std::nullopt;
}

template <Cache::Strategy S>
void FileReader<S>::Announce(ULONGLONG offset, ULONGLONG length) const {
  Bench::Detail::RecordPrefetch(offset, length);
  reader_->Prefetch(offset, length);
}

template <Cache::Strategy S>
void FileReader<S>::AnnounceStanding(ULONGLONG offset, ULONGLONG length) const {
  Bench::Detail::RecordStandingPrefetch(offset, length);
  reader_->PrefetchStanding(offset, length);
}

template <Cache::Strategy S>
void FileReader<S>::Prefetch(ULONGLONG offset, ULONGLONG length) const {
  AnnounceRuns(offset, length, false);
}

template <Cache::Strategy S>
void FileReader<S>::PrefetchStanding(ULONGLONG offset, ULONGLONG length) const {
  AnnounceRuns(offset, length, true);
}

template <Cache::Strategy S>
void FileReader<S>::AnnounceRuns(ULONGLONG offset, ULONGLONG length,
                                 bool standing) const {
  if (!reader_ || length == 0) {
    return;
  }
  if constexpr (S == Cache::Strategy::FullCache) {
    const auto block = static_cast<ULONGLONG>(block_bytes_);
    // The blocks MUST end below the largest address the cache reads at.
    const auto max_end =
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
      AnnounceOne(first, end - first, standing);
      return;
    }
    AnnounceUncachedBlocks(first, end, standing);
  } else {
    AnnounceOne(offset, length, standing);
  }
}

template <Cache::Strategy S>
void FileReader<S>::AnnounceOne(ULONGLONG offset, ULONGLONG length,
                                bool standing) const {
  if (standing) {
    AnnounceStanding(offset, length);
  } else {
    Announce(offset, length);
  }
}

// The cache serves a block it holds without a read, so only the other blocks
// of [first, end) are announced: a range nothing reads would be read for
// nothing. FullCache only, with first and end on block boundaries.
template <Cache::Strategy S>
void FileReader<S>::AnnounceUncachedBlocks(ULONGLONG first, ULONGLONG end,
                                           bool standing) const {
  const auto block = static_cast<ULONGLONG>(block_bytes_);
  std::optional<ULONGLONG> run_start;
  for (ULONGLONG at = first; at < end; at += block) {
    const bool cached = cache_->Contains(at / block);
    if (cached && run_start) {
      AnnounceOne(*run_start, at - *run_start, standing);
      run_start.reset();
    } else if (!cached && !run_start) {
      run_start = at;
    }
  }
  if (run_start) {
    AnnounceOne(*run_start, end - *run_start, standing);
  }
}

template <Cache::Strategy S>
Tuning::MediaKind FileReader<S>::GetMediaKind() const {
  return reader_ ? reader_->GetMediaKind() : Tuning::MediaKind::Unknown;
}

template <Cache::Strategy S>
void FileReader<S>::CancelPrefetch() const {
  if (reader_) {
    Bench::Detail::RecordCancel();
    reader_->CancelPrefetch();
  }
}

template <Cache::Strategy S>
void FileReader<S>::CancelStanding(ULONGLONG offset, ULONGLONG length) const {
  if (!reader_ || length == 0) {
    return;
  }
  if constexpr (S == Cache::Strategy::FullCache) {
    constexpr ULONGLONG max_value = (std::numeric_limits<ULONGLONG>::max)();
    const auto block = static_cast<ULONGLONG>(block_bytes_);
    const ULONGLONG first = offset - offset % block;
    // An interval that runs to the largest offset is cut there.
    ULONGLONG end = max_value;
    if (offset <= max_value - (block - 1) &&
        length <= max_value - (block - 1) - offset) {
      const ULONGLONG last = offset + length + block - 1;
      end = last - last % block;
    }
    Bench::Detail::RecordStandingCancel(first, end - first);
    reader_->CancelStanding(first, end - first);
  } else {
    Bench::Detail::RecordStandingCancel(offset, length);
    reader_->CancelStanding(offset, length);
  }
}

template <Cache::Strategy S>
void FileReader<S>::SetReadPattern(Tuning::Pattern pattern) const {
  if (reader_) {
    Bench::Detail::RecordPattern(pattern);
    reader_->SetReadPattern(pattern);
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

  if (!ReadFromReader(addr, std::span<BYTE>{buffer_.data(), length})) {
    Log::Error("Cannot read file at adress {}", addr.QuadPart);
    return {};
  }

  return std::span<const BYTE>{buffer_.data(), length};
}

// Reads exactly "length" bytes at "addr" into a buffer that only the returned
// view owns, bypassing the block cache. FullCache falls back to it when a
// whole block cannot be read: the block may extend past the end of the
// medium. The short block MUST NOT be cached as if it were complete.
template <Cache::Strategy S>
std::optional<Cache::View> FileReader<S>::ReadUncached(LARGE_INTEGER addr,
                                                       DWORD length) const {
  const Cache::BlockCache::Stitched exact = cache_->NewStitched(length);
  if (!ReadFromReader(addr, exact.bytes)) {
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

  // The blocks the range needs, and the run of missing ones that one disk read
  // brings in.
  const auto last_index =
      static_cast<std::uint64_t>((addr.QuadPart + length - 1) / block_bytes_);
  const auto read = [this](LARGE_INTEGER& at, std::span<BYTE> dest) {
    return ReadFromReader(at, dest);
  };
  RunBlockLoader loader{*cache_, block_bytes_, max_run_bytes_, last_index,
                        read};
  const auto get_block = [&loader](LARGE_INTEGER block_addr) {
    return loader.Get(block_addr);
  };

  const bool crosses_block = addr.QuadPart / block_bytes_ !=
                             (addr.QuadPart + length - 1) / block_bytes_;

  if (!crosses_block) {
    // Fast path: the request fits in a single block; return a zero-copy view.
    const LARGE_INTEGER block_addr{.QuadPart = addr.QuadPart -
                                               addr.QuadPart % block_bytes_};
    const std::shared_ptr<Cache::Block> block = get_block(block_addr);
    if (!block) {
      return ReadUncached(addr, length);
    }

    const std::span<const BYTE> bytes =
        std::span<const BYTE>{block->bytes.get(),
                              static_cast<size_t>(block_bytes_)}
            .subspan(gsl::narrow<size_t>(addr.QuadPart % block_bytes_), length);
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
                                               cur.QuadPart % block_bytes_};
    const std::shared_ptr<Cache::Block> block = get_block(block_addr);
    if (!block) {
      return ReadUncached(addr, length);
    }

    CopyBlockPart(*block, block_bytes_, out, cur, remaining);
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
