#include "counter-text.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <span>
#include <string_view>

#include <gsl/narrow>

#include <ntfs-browser/namespace.h>

#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen {

namespace {

// The words before and after the number of a line.
constexpr std::string_view line_head = "This is line ";
constexpr std::string_view line_tail = ".\r\n";

// The base of the line numbers.
constexpr ULONGLONG decimal_base = 10;

// Room for one whole line: the head, the 20 digits of the largest 64-bit
// number, and the tail.
constexpr std::size_t line_buffer_bytes = 48;

// Bytes of a line that has the given number of digits.
ULONGLONG LineBytes(ULONGLONG digits) {
  return line_head.size() + digits + line_tail.size();
}

// The number of the line that holds offset, and the offset of that line.
struct Position {
  ULONGLONG number;
  ULONGLONG start;
};

// Finds the line that holds offset. The numbers with d digits are 10^(d-1) to
// 10^d - 1, but the one digit numbers start at 0, and each line takes
// LineBytes(d).
Position Locate(ULONGLONG offset) {
  ULONGLONG first = 0;
  ULONGLONG count = decimal_base;
  ULONGLONG start = 0;
  for (ULONGLONG digits = 1;; ++digits) {
    const ULONGLONG block = count * LineBytes(digits);
    if (offset - start < block) {
      const ULONGLONG index = (offset - start) / LineBytes(digits);
      return {.number = first + index,
              .start = start + (index * LineBytes(digits))};
    }
    start += block;
    first = digits == 1 ? decimal_base : first * decimal_base;
    count = first * (decimal_base - 1);
  }
}

}  // namespace

void CounterText::Fill(ULONGLONG offset, std::span<BYTE> out) {
  const Position here = Locate(offset);
  ULONGLONG number = here.number;
  ULONGLONG skip = offset - here.start;
  std::size_t done = 0;
  while (done < out.size()) {
    std::array<char, line_buffer_bytes> line{};
    char* end = std::ranges::copy(line_head, line.data()).out;
    end = std::to_chars(end, line.data() + line.size(), number).ptr;
    end = std::ranges::copy(line_tail, end).out;
    const std::size_t length = gsl::narrow<std::size_t>(end - line.data());
    const std::size_t take =
        std::min(length - gsl::narrow<std::size_t>(skip), out.size() - done);
    for (std::size_t i = 0; i < take; ++i) {
      out[done + i] =
          static_cast<BYTE>(line[gsl::narrow<std::size_t>(skip) + i]);
    }
    done += take;
    skip = 0;
    ++number;
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tool::FuzzGen
