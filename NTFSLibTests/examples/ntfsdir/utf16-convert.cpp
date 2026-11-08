#include "utf16-convert.h"

#include <cstddef>
#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir {

#ifdef _WIN32

std::u16string ToUtf16(std::wstring_view wide) {
  return {wide.begin(), wide.end()};
}

std::wstring FromUtf16(std::u16string_view units) {
  return {units.begin(), units.end()};
}

#else

namespace {

// The first code point that needs a surrogate pair.
constexpr char32_t first_supplementary = 0x10000;
// The last code point of Unicode.
constexpr char32_t last_code_point = 0x10FFFF;
// The replacement character, for a value that is no code point.
constexpr char16_t replacement = 0xFFFD;
// Surrogate ranges: high 0xD800 to 0xDBFF, low 0xDC00 to 0xDFFF.
constexpr char16_t high_surrogate_first = 0xD800;
constexpr char16_t low_surrogate_first = 0xDC00;
constexpr char16_t low_surrogate_last = 0xDFFF;
// A surrogate carries 10 bits of the code point.
constexpr unsigned surrogate_bits = 10;
// The mask of those 10 bits.
constexpr char32_t surrogate_mask = 0x3FF;

// Whether unit is a high surrogate, which opens a pair.
bool IsHigh(char16_t unit) {
  return unit >= high_surrogate_first && unit < low_surrogate_first;
}

// Whether unit is a low surrogate, which closes a pair.
bool IsLow(char16_t unit) {
  return unit >= low_surrogate_first && unit <= low_surrogate_last;
}

}  // namespace

std::u16string ToUtf16(std::wstring_view wide) {
  std::u16string units;
  for (const wchar_t wide_char : wide) {
    const auto point = static_cast<char32_t>(wide_char);
    if (point > last_code_point) {
      units.push_back(replacement);
    } else if (point >= first_supplementary) {
      const char32_t offset = point - first_supplementary;
      units.push_back(static_cast<char16_t>(high_surrogate_first +
                                            (offset >> surrogate_bits)));
      units.push_back(static_cast<char16_t>(low_surrogate_first +
                                            (offset & surrogate_mask)));
    } else {
      units.push_back(static_cast<char16_t>(point));
    }
  }
  return units;
}

std::wstring FromUtf16(std::u16string_view units) {
  std::wstring wide;
  for (std::size_t at = 0; at < units.size(); at++) {
    const char16_t unit = units[at];
    if (IsHigh(unit) && at + 1 < units.size() && IsLow(units[at + 1])) {
      const char32_t high = unit - high_surrogate_first;
      const char32_t low = units[at + 1] - low_surrogate_first;
      wide.push_back(static_cast<wchar_t>(first_supplementary +
                                          ((high << surrogate_bits) | low)));
      at++;
    } else {
      wide.push_back(static_cast<wchar_t>(unit));
    }
  }
  return wide;
}

#endif

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir
