#include "output.h"

#include <cstdio>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>

#ifdef _WIN32
  #include <array>
  #include <climits>
  #include <cstddef>
  #include <cwchar>

  #include <fcntl.h>
  #include <io.h>
#else
  #include "text-convert.h"
#endif

namespace NtfsDevice {

#ifdef _WIN32

namespace {

// The code unit below which every locale of the CRT keeps a character as one
// byte of the same value: ASCII.
constexpr wchar_t ascii_limit = 0x80;

}  // namespace

#endif

Output::Output() {
  buffer_.reserve(output_flush_bytes);
  writing_.reserve(output_flush_bytes);
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
  std::mbstate_t state{};
  std::array<char, MB_LEN_MAX> bytes{};
  for (const wchar_t unit : text) {
    if (unit == L'\n') {
      buffer_.append(line_end);
      continue;
    }
    if (unit < ascii_limit) {
      buffer_.push_back(static_cast<char>(unit));
      continue;
    }
    // A character the locale cannot hold ends the text, as it ended fprintf().
    size_t length = 0;
    if (wcrtomb_s(&length, bytes.data(), bytes.size(), unit, &state) != 0) {
      break;
    }
    buffer_.append(bytes.data(), length);
  }
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
