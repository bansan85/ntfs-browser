#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include <gsl/narrow>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/read-ahead-disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

namespace NTFS_BROWSER_NAMESPACE::Io {

namespace {

// The largest sector size NTFS supports. A chunk is a multiple of it, since a
// raw device only accepts reads of whole sectors.
constexpr std::uint64_t chunk_alignment = 4096;

// Ceiling of a chunk, however big the caller's reads are: two of them are held
// at once.
constexpr std::uint64_t max_chunk_bytes = 64ULL * 1024 * 1024;

// Chunks read ahead at once. Two make a double buffer: the caller works on one
// while the worker reads the other.
constexpr size_t window_chunks = 2;

// Rounds value up to a multiple of chunk_alignment.
std::uint64_t RoundUp(std::uint64_t value) {
  return (value + chunk_alignment - 1) / chunk_alignment * chunk_alignment;
}

}  // namespace

// One piece of the medium, read by the worker for the caller.
struct ReadAheadChunk {
  enum class State { Queued, Reading, Ready, Failed };

  ReadAheadChunk(LONGLONG addr, size_t size, std::unique_ptr<BYTE[]> data)
      : addr(addr), size(size), data(std::move(data)) {}

  LONGLONG addr;
  size_t size;
  std::unique_ptr<BYTE[]> data;

  // Guarded by the mutex of the Impl that owns the chunk.
  State state{State::Queued};

  // Set when the caller no longer wants the chunk. A worker that has not begun
  // the read then skips it.
  std::atomic<bool> cancelled{false};
};

class ReadAheadDiskReader::Impl {
 public:
  Impl(std::unique_ptr<IDiskReader> inner, const ReadAheadOptions& options)
      : inner_(std::move(inner)),
        chunk_bytes_(
            RoundUp(std::min(std::max(options.chunk_bytes, std::uint64_t{1}),
                             max_chunk_bytes))) {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  ~Impl() {
    {
      const std::scoped_lock lock{mutex_};
      stop_ = true;
      DropWindow();
    }
    work_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  bool Open(std::wstring_view path) {
    {
      const std::scoped_lock lock{mutex_};
      DropWindow();
      has_last_ = false;
      prefetch_floor_ = 0;
    }

    const std::scoped_lock inner_lock{inner_mutex_};
    return inner_ != nullptr && inner_->Open(path);
  }

  bool ReadInto(LONGLONG start, std::span<BYTE> dest);

  // Size of the wrapped reader. It reads nothing.
  std::optional<ULONGLONG> Size() {
    const std::scoped_lock inner_lock{inner_mutex_};
    return inner_ != nullptr ? inner_->Size() : std::nullopt;
  }

 private:
  using Chunk = ReadAheadChunk;

  // Reads from the wrapped reader on the calling thread, once the worker is
  // done with it.
  bool DirectRead(LONGLONG addr, std::span<BYTE> dest);

  // Copies the leading bytes of the request that the window holds, waiting for
  // a chunk still in flight. Stops at the first byte it cannot serve, or at a
  // chunk whose read failed, which it drops with the rest of the window.
  // Returns the number of bytes copied.
  size_t ServeFromWindow(std::unique_lock<std::mutex>& lock, LONGLONG start,
                         std::span<BYTE> dest);

  // Forgets every chunk. A chunk the worker has not begun is never read.
  void DropWindow();

  // Forgets the chunks the caller has read past.
  void TrimWindow();

  // Queues chunks after the last byte the caller read, until the window is
  // full.
  void TopUp();

  // Starts the worker on first use. False when no thread can be had, in which
  // case the decorator only forwards.
  bool EnsureWorker();

  // The size of the next chunk: the configured one, or the caller's last read
  // if that is bigger, so that one read is one chunk.
  [[nodiscard]] std::uint64_t ChunkSize() const;

  void WorkerMain();

  // Reads one chunk through the wrapped reader.
  bool ReadChunk(Chunk& chunk);

  // The first chunk the worker has yet to read, or null.
  [[nodiscard]] std::shared_ptr<Chunk> NextQueued() const;

  // Held by whoever calls the wrapped reader: the worker for a chunk, the
  // caller for a direct read. The wrapped reader may keep a file position.
  std::mutex inner_mutex_;
  std::unique_ptr<IDiskReader> inner_;

  const std::uint64_t chunk_bytes_;

  // Guards everything below, and the state of every chunk. Never held while
  // calling the wrapped reader, and never taken while inner_mutex_ is held.
  std::mutex mutex_;
  std::condition_variable work_;
  std::condition_variable done_;
  bool stop_{false};

  // The chunks read ahead, in address order, each one starting where the
  // previous ends. Only the caller's thread changes the list.
  std::deque<std::shared_ptr<Chunk>> window_;

  // Where the previous read ended, and how big it was. Without a previous read,
  // no read is a forward one.
  bool has_last_{false};
  LONGLONG last_end_{0};
  std::uint64_t last_size_{0};

  // No chunk starts below this address. Set past a chunk whose read failed, so
  // that a bad place is not read ahead again for every read that nears it.
  LONGLONG prefetch_floor_{0};

  // Declared last: it starts only once everything above is built.
  std::thread worker_;
};

bool ReadAheadDiskReader::Impl::DirectRead(LONGLONG addr,
                                           std::span<BYTE> dest) {
  const std::scoped_lock lock{inner_mutex_};
  if (inner_ == nullptr) {
    return false;
  }

  LARGE_INTEGER at{.QuadPart = addr};
  return inner_->ReadInto(at, dest);
}

void ReadAheadDiskReader::Impl::DropWindow() {
  for (const std::shared_ptr<Chunk>& chunk : window_) {
    chunk->cancelled = true;
  }
  window_.clear();
}

void ReadAheadDiskReader::Impl::TrimWindow() {
  while (!window_.empty() &&
         window_.front()->addr + gsl::narrow<LONGLONG>(window_.front()->size) <=
             last_end_) {
    window_.front()->cancelled = true;
    window_.pop_front();
  }
}

std::uint64_t ReadAheadDiskReader::Impl::ChunkSize() const {
  return RoundUp(std::min(std::max(chunk_bytes_, last_size_), max_chunk_bytes));
}

bool ReadAheadDiskReader::Impl::EnsureWorker() {
  if (worker_.joinable()) {
    return true;
  }

  try {
    worker_ = std::thread([this] { WorkerMain(); });
  } catch (...) {
    return false;
  }
  return true;
}

void ReadAheadDiskReader::Impl::TopUp() {
  const std::uint64_t size = ChunkSize();
  bool queued = false;
  while (window_.size() < window_chunks) {
    const LONGLONG next = window_.empty()
                              ? last_end_
                              : window_.back()->addr +
                                    gsl::narrow<LONGLONG>(window_.back()->size);
    // Past LLONG_MAX there is no address to read at.
    if (next < prefetch_floor_ ||
        std::cmp_greater(size, std::numeric_limits<LONGLONG>::max() - next)) {
      break;
    }
    if (!EnsureWorker()) {
      break;
    }

    // A failed allocation only costs the read ahead.
    BYTE* const raw = new (std::nothrow) BYTE[size];
    if (raw == nullptr) {
      break;
    }
    window_.push_back(std::make_shared<Chunk>(next, gsl::narrow<size_t>(size),
                                              std::unique_ptr<BYTE[]>(raw)));
    queued = true;
  }

  if (queued) {
    work_.notify_one();
  }
}

std::shared_ptr<ReadAheadChunk> ReadAheadDiskReader::Impl::NextQueued() const {
  for (const std::shared_ptr<Chunk>& chunk : window_) {
    if (chunk->state == Chunk::State::Queued) {
      return chunk;
    }
  }
  return nullptr;
}

bool ReadAheadDiskReader::Impl::ReadChunk(Chunk& chunk) {
  const std::scoped_lock lock{inner_mutex_};
  // Dropped while the worker waited for the wrapped reader.
  if (chunk.cancelled || inner_ == nullptr) {
    return false;
  }

  try {
    LARGE_INTEGER at{.QuadPart = chunk.addr};
    return inner_->ReadInto(at, std::span<BYTE>{chunk.data.get(), chunk.size});
  } catch (...) {
    // An exception must not end the process from a thread of its own. The
    // caller's direct read meets it again, and reports it there.
    return false;
  }
}

void ReadAheadDiskReader::Impl::WorkerMain() {
  Log::Detail::SilenceCurrentThread();

  std::unique_lock lock{mutex_};
  for (;;) {
    std::shared_ptr<Chunk> job;
    work_.wait(lock, [&] {
      job = NextQueued();
      return stop_ || job != nullptr;
    });
    if (stop_) {
      return;
    }

    job->state = Chunk::State::Reading;
    lock.unlock();
    const bool ok = ReadChunk(*job);
    lock.lock();
    job->state = ok ? Chunk::State::Ready : Chunk::State::Failed;
    done_.notify_all();
  }
}

size_t ReadAheadDiskReader::Impl::ServeFromWindow(
    std::unique_lock<std::mutex>& lock, LONGLONG start, std::span<BYTE> dest) {
  size_t copied = 0;
  while (copied < dest.size()) {
    const LONGLONG pos = start + gsl::narrow<LONGLONG>(copied);
    const auto found = std::ranges::find_if(
        window_, [pos](const std::shared_ptr<Chunk>& chunk) {
          return pos >= chunk->addr &&
                 pos < chunk->addr + gsl::narrow<LONGLONG>(chunk->size);
        });
    if (found == window_.end()) {
      break;
    }

    const std::shared_ptr<Chunk> chunk = *found;
    done_.wait(lock, [&] {
      return chunk->state == Chunk::State::Ready ||
             chunk->state == Chunk::State::Failed;
    });
    if (chunk->state == Chunk::State::Failed) {
      prefetch_floor_ = chunk->addr + gsl::narrow<LONGLONG>(chunk->size);
      DropWindow();
      break;
    }

    const std::span<const BYTE> bytes{chunk->data.get(), chunk->size};
    const size_t offset = gsl::narrow<size_t>(pos - chunk->addr);
    const size_t count = std::min(dest.size() - copied, bytes.size() - offset);
    std::ranges::copy(bytes.subspan(offset, count),
                      dest.subspan(copied).begin());
    copied += count;
  }
  return copied;
}

bool ReadAheadDiskReader::Impl::ReadInto(LONGLONG start, std::span<BYTE> dest) {
  // Nothing to stream. The wrapped reader judges such a request.
  if (dest.empty() || start < 0 ||
      std::cmp_greater(dest.size(),
                       std::numeric_limits<LONGLONG>::max() - start)) {
    return DirectRead(start, dest);
  }

  std::unique_lock lock{mutex_};
  const size_t copied = ServeFromWindow(lock, start, dest);
  const bool forward = copied > 0 || (has_last_ && start == last_end_);

  if (copied < dest.size()) {
    // A read the window cannot serve at all makes the window useless.
    if (copied == 0) {
      DropWindow();
    }
    lock.unlock();
    const bool ok =
        DirectRead(start + gsl::narrow<LONGLONG>(copied), dest.subspan(copied));
    lock.lock();
    if (!ok) {
      DropWindow();
      has_last_ = false;
      return false;
    }
  }

  has_last_ = true;
  last_end_ = start + gsl::narrow<LONGLONG>(dest.size());
  last_size_ = dest.size();
  if (forward) {
    TrimWindow();
    TopUp();
  }
  return true;
}

ReadAheadDiskReader::ReadAheadDiskReader(std::unique_ptr<IDiskReader> inner,
                                         const ReadAheadOptions& options)
    : impl_(std::make_unique<Impl>(std::move(inner), options)) {}

ReadAheadDiskReader::~ReadAheadDiskReader() = default;

bool ReadAheadDiskReader::Open(std::wstring_view path) {
  return impl_->Open(path);
}

bool ReadAheadDiskReader::ReadInto(LARGE_INTEGER& addr,
                                   std::span<BYTE> dest) const {
  return impl_->ReadInto(addr.QuadPart, dest);
}

std::optional<ULONGLONG> ReadAheadDiskReader::Size() const {
  return impl_->Size();
}

}  // namespace NTFS_BROWSER_NAMESPACE::Io
