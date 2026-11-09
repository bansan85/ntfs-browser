#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess {

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

// Text of a listing, built piece by piece in memory. The lines are built by
// hand: std::format() would cost more than the rest of a line. It never writes
// anything: Output hands what it holds to stdout, and a thread that formats a
// part of a listing for Output to append uses one of its own.
class LineBuffer {
 public:
  // Appends one piece of the listing, formatted as std::format() does.
  template <class... Args>
  void Print(std::format_string<Args...> fmt, Args&&... args) {
    std::format_to(std::back_inserter(buffer_), fmt,
                   std::forward<Args>(args)...);
  }

  // Same, then ends the line.
  template <class... Args>
  void PrintLine(std::format_string<Args...> fmt, Args&&... args) {
    Print(fmt, std::forward<Args>(args)...);
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
  void EndLine() { buffer_.append(line_end); }

  // The text built so far.
  [[nodiscard]] std::string_view Text() const noexcept { return buffer_; }

  // Forgets the text, and keeps the memory for the next.
  void Clear() noexcept { buffer_.clear(); }

 protected:
  // Appends the fill a text of length characters needs to span width columns.
  void Pad(size_t length, size_t width, char fill) {
    if (length < width) {
      buffer_.append(width - length, fill);
    }
  }

  std::string buffer_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess
