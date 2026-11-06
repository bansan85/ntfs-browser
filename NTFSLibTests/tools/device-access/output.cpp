#include "output.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <system_error>
#include <thread>

#include <ntfs-browser/namespace.h>

#ifdef _WIN32
  #include <cstddef>

  #include <fcntl.h>
  #include <io.h>
#endif

#include <ntfs-browser/bench/bench.h>

namespace NtfsDevice {

namespace {

// The most a chunk reserves up front. A chunk of 1 GiB, which the tuning
// allows, would otherwise take 2 GiB at once and fail in a 32-bit process.
constexpr size_t max_reserved_bytes = size_t{64} * 1024 * 1024;

}  // namespace

Output::Output(size_t flush_bytes, std::FILE* stream)
    : flush_bytes_(flush_bytes != 0 ? flush_bytes : output_flush_bytes),
      stream_(stream) {
  buffer_.reserve((std::min)(flush_bytes_, max_reserved_bytes));
  writing_.reserve((std::min)(flush_bytes_, max_reserved_bytes));
#ifdef _WIN32
  if (stream_ == stdout) {
    fflush(stdout);
    text_mode_ = _setmode(_fileno(stdout), _O_BINARY);
  }
#endif
  try {
    writer_ = std::thread([this] { WriteChunks(); });
  } catch (const std::system_error&) {
    // Without a thread, Flush() writes each chunk itself.
  }
}

Output::~Output() {
  Flush();
  StopWriter();
#ifdef _WIN32
  if (stream_ == stdout) {
    fflush(stdout);
    if (text_mode_ != -1) {
      _setmode(_fileno(stdout), text_mode_);
    }
  }
#endif
}

void Output::Flush() {
  if (buffer_.empty()) {
    return;
  }
  if (!writer_.joinable()) {
    fwrite(buffer_.data(), 1, buffer_.size(), stream_);
    buffer_.clear();
    return;
  }

  std::unique_lock lock{mutex_};
  written_.wait(lock, [this] { return !chunk_ready_; });
  // The writer left its buffer empty: it becomes the one to fill.
  buffer_.swap(writing_);
  chunk_ready_ = true;
  lock.unlock();
  ready_.notify_one();
}

void Output::WriteChunks() {
  const NTFS_BROWSER_NAMESPACE::Bench::ThreadScope cpu(
      NTFS_BROWSER_NAMESPACE::Bench::ThreadRole::OutputWriter);
  std::unique_lock lock{mutex_};
  for (;;) {
    ready_.wait(lock, [this] { return chunk_ready_ || stopping_; });
    if (!chunk_ready_) {
      return;
    }
    // Until chunk_ready_ is cleared, the chunk belongs to this thread alone.
    lock.unlock();
    fwrite(writing_.data(), 1, writing_.size(), stream_);
    writing_.clear();
    lock.lock();
    chunk_ready_ = false;
    written_.notify_one();
  }
}

void Output::StopWriter() {
  if (!writer_.joinable()) {
    return;
  }
  {
    const std::scoped_lock lock{mutex_};
    stopping_ = true;
  }
  ready_.notify_one();
  writer_.join();
}

}  // namespace NtfsDevice
