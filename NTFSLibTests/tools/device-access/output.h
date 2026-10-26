#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <format>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace NtfsDevice {

// Bytes of a listing gathered before stdout gets them. 1 MiB turns a listing of
// millions of lines into a few dozen writes.
inline constexpr size_t output_flush_bytes = size_t{1024} * 1024;

// Digits of the largest ULONGLONG.
inline constexpr size_t max_decimal_digits = 20;

#ifdef _WIN32
// The end of a line of a listing. stdout is binary while it is written, so the
// listing adds the CR the text mode would: that mode converts a few KiB at a
// time, and each chunk costs a write.
inline constexpr std::string_view line_end = "\r\n";
#else
inline constexpr std::string_view line_end = "\n";
#endif

// Gathers a listing, and hands it to stdout in big chunks. A chunk ends between
// two pieces of text, so a console never gets a UTF-8 sequence cut in two. The
// lines are built by hand: std::format() would cost more than the rest of a
// line. A thread of its own writes each chunk while the next one is built:
// writing a big listing costs about as much as building it.
//
// While one lives, stdout is binary on Windows and nothing else MAY write to
// it: the listing goes through this object alone, and ends its own lines.
class Output {
 public:
  Output();
  Output(const Output&) = delete;
  Output(Output&&) = delete;
  Output& operator=(const Output&) = delete;
  Output& operator=(Output&&) = delete;
  // Writes what is left, and gives stdout back its mode.
  ~Output();

  // Appends one piece of the listing, formatted as std::format() does.
  template <class... Args>
  void Print(std::format_string<Args...> fmt, Args&&... args) {
    std::format_to(std::back_inserter(buffer_), fmt,
                   std::forward<Args>(args)...);
    FlushIfFull();
  }

  // Same, then ends the line.
  template <class... Args>
  void PrintLine(std::format_string<Args...> fmt, Args&&... args) {
    std::format_to(std::back_inserter(buffer_), fmt,
                   std::forward<Args>(args)...);
    EndLine();
  }

  void Append(std::string_view text) { buffer_.append(text); }

  // Appends a name or a message, in the encoding WriteText() gives it: UTF-8,
  // whatever the locale or the code page, so a name never stops at a character
  // that the code page lacks. An unpaired surrogate is written in its WTF-8
  // form (see WideToUtf8()). On Windows a line feed in it becomes the CR LF the
  // text mode made.
  void AppendText(std::wstring_view text);

  // Appends text right-aligned in width columns, or left-aligned.
  void AppendRight(std::string_view text, size_t width) {
    Pad(text.size(), width, ' ');
    buffer_.append(text);
  }

  void AppendLeft(std::string_view text, size_t width) {
    buffer_.append(text);
    Pad(text.size(), width, ' ');
  }

  // Appends value in decimal, right-aligned in width columns filled with fill.
  void AppendNumber(ULONGLONG value, size_t width, char fill = ' ') {
    std::array<char, max_decimal_digits> digits{};
    const char* end =
        std::to_chars(digits.data(), digits.data() + digits.size(), value).ptr;
    const auto length = static_cast<size_t>(end - digits.data());
    Pad(length, width, fill);
    buffer_.append(digits.data(), length);
  }

  // Ends the current line.
  void EndLine() {
    buffer_.append(line_end);
    FlushIfFull();
  }

  // Hands what is gathered so far to the writer, once it has written the
  // previous chunk.
  void Flush();

 private:
  // Body of the writer thread: writes each chunk Flush() hands over, until
  // StopWriter() and the last chunk are done.
  void WriteChunks();

  // Lets the writer finish the chunk it has, then ends it.
  void StopWriter();

  // Appends the fill a text of length characters needs to span width columns.
  void Pad(size_t length, size_t width, char fill) {
    if (length < width) {
      buffer_.append(width - length, fill);
    }
  }

  void FlushIfFull() {
    if (buffer_.size() >= output_flush_bytes) {
      Flush();
    }
  }

  std::string buffer_;
#ifdef _WIN32
  // The mode stdout had before, restored once the listing is written. -1 when
  // it could not be changed.
  int text_mode_{-1};
#endif

  // Guards everything below but the thread itself.
  std::mutex mutex_;
  // Signalled when a chunk waits for the writer, and when it has written one.
  std::condition_variable ready_;
  std::condition_variable written_;
  // The chunk the writer writes, and whether it holds one to write.
  std::string writing_;
  bool chunk_ready_{false};
  bool stopping_{false};
  // Not joinable when no thread could be had. Declared last: it starts once
  // everything above is built.
  std::thread writer_;
};

}  // namespace NtfsDevice
