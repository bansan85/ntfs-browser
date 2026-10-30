#include "output.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
  #include <cstddef>

  #include <fcntl.h>
  #include <io.h>
#endif

#include <ntfs-browser/bench/bench.h>

#include "text-convert.h"

namespace NtfsDevice {

namespace {

// The most a chunk reserves up front. A chunk of 1 GiB, which the tuning
// allows, would otherwise take 2 GiB at once and fail in a 32-bit process.
constexpr size_t max_reserved_bytes = size_t{64} * 1024 * 1024;

}  // namespace

Output::Output(size_t flush_bytes)
    : flush_bytes_(flush_bytes != 0 ? flush_bytes : output_flush_bytes) {
  buffer_.reserve((std::min)(flush_bytes_, max_reserved_bytes));
  writing_.reserve((std::min)(flush_bytes_, max_reserved_bytes));
#ifdef _WIN32
  fflush(stdout);
  text_mode_ = _setmode(_fileno(stdout), _O_BINARY);
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
  fflush(stdout);
  if (text_mode_ != -1) {
    _setmode(_fileno(stdout), text_mode_);
  }
#endif
}

void Output::AppendText(std::wstring_view text) {
#ifdef _WIN32
  // stdout is binary here: a line feed in a name becomes the CR LF the text
  // mode made.
  size_t start = 0;
  for (size_t line_feed = text.find(L'\n');
       line_feed != std::wstring_view::npos;
       line_feed = text.find(L'\n', start)) {
    AppendAsUtf8(buffer_, text.substr(start, line_feed - start));
    buffer_.append(line_end);
    start = line_feed + 1;
  }
  AppendAsUtf8(buffer_, text.substr(start));
#else
  AppendAsUtf8(buffer_, text);
#endif
  FlushIfFull();
}

void Output::Flush() {
  if (buffer_.empty()) {
    return;
  }
  if (!writer_.joinable()) {
    fwrite(buffer_.data(), 1, buffer_.size(), stdout);
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
    fwrite(writing_.data(), 1, writing_.size(), stdout);
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
