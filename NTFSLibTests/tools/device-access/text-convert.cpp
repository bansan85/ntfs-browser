#include "text-convert.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include <gsl/narrow>

namespace NtfsDevice {

namespace {

// UTF-16 surrogate ranges: a high surrogate starts a pair, a low one ends it.
constexpr char32_t high_surrogate_first = 0xD800;
constexpr char32_t high_surrogate_last = 0xDBFF;
constexpr char32_t low_surrogate_first = 0xDC00;
constexpr char32_t low_surrogate_last = 0xDFFF;
// First code point outside the Basic Multilingual Plane: what a pair encodes
// from, and the offset its two halves are summed against.
constexpr char32_t supplementary_base = 0x10000;
// Bits one surrogate carries of the code point.
constexpr unsigned surrogate_payload_bits = 10;
// Largest Unicode code point.
constexpr char32_t max_code_point = 0x10FFFF;
// U+FFFD REPLACEMENT CHARACTER, for a value no UTF-8 sequence can hold.
constexpr char32_t replacement_character = 0xFFFD;

// Largest code point of a 1-, 2- and 3-byte UTF-8 sequence.
constexpr char32_t utf8_max1 = 0x7F;
constexpr char32_t utf8_max2 = 0x7FF;
constexpr char32_t utf8_max3 = 0xFFFF;

// UTF-8 lead bytes, per sequence length: the pattern the length tags (tag).
constexpr unsigned utf8_lead2_tag = 0xC0U;
constexpr unsigned utf8_lead3_tag = 0xE0U;
constexpr unsigned utf8_lead4_tag = 0xF0U;

// UTF-8 continuation byte: the pattern it holds (tag), the bits left for the
// code point, and how many there are.
constexpr unsigned utf8_cont_tag = 0x80U;
constexpr unsigned utf8_cont_payload = 0x3FU;
constexpr unsigned utf8_cont_bits = 6U;

bool IsHighSurrogate(char32_t unit) {
  return unit >= high_surrogate_first && unit <= high_surrogate_last;
}

bool IsLowSurrogate(char32_t unit) {
  return unit >= low_surrogate_first && unit <= low_surrogate_last;
}

// The continuation byte that carries the payload bits of code_point from
// shift up.
char ContinuationByte(char32_t code_point, unsigned shift) {
  return static_cast<char>(utf8_cont_tag |
                           ((code_point >> shift) & utf8_cont_payload));
}

// Appends code_point to out as UTF-8. A surrogate code point gets the
// 3-byte form, which is not valid UTF-8 but is what WTF-8 defines.
void AppendUtf8(std::string& out, char32_t code_point) {
  if (code_point > max_code_point) {
    code_point = replacement_character;
  }

  if (code_point <= utf8_max1) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= utf8_max2) {
    out.push_back(
        static_cast<char>(utf8_lead2_tag | (code_point >> utf8_cont_bits)));
    out.push_back(ContinuationByte(code_point, 0));
  } else if (code_point <= utf8_max3) {
    out.push_back(static_cast<char>(utf8_lead3_tag |
                                    (code_point >> (2U * utf8_cont_bits))));
    out.push_back(ContinuationByte(code_point, utf8_cont_bits));
    out.push_back(ContinuationByte(code_point, 0));
  } else {
    out.push_back(static_cast<char>(utf8_lead4_tag |
                                    (code_point >> (3U * utf8_cont_bits))));
    out.push_back(ContinuationByte(code_point, 2U * utf8_cont_bits));
    out.push_back(ContinuationByte(code_point, utf8_cont_bits));
    out.push_back(ContinuationByte(code_point, 0));
  }
}

#ifndef _WIN32

// UTF-8 lead byte of a 1-byte sequence: bit 7 clear.
constexpr unsigned utf8_ascii_bit = 0x80U;

// UTF-8 lead bytes, per sequence length: the bits that identify the length
// (mask), and the bits left for the code point.
constexpr unsigned utf8_lead2_mask = 0xE0U;
constexpr unsigned utf8_lead2_payload = 0x1FU;
constexpr unsigned utf8_lead3_mask = 0xF0U;
constexpr unsigned utf8_lead3_payload = 0x0FU;
constexpr unsigned utf8_lead4_mask = 0xF8U;
constexpr unsigned utf8_lead4_payload = 0x07U;

// UTF-8 continuation byte: the bits that identify it.
constexpr unsigned utf8_cont_mask = 0xC0U;

#endif  // !_WIN32

}  // namespace

std::string WideToUtf8(std::wstring_view wide) {
  std::string out;
  out.reserve(wide.size());

  for (size_t position = 0; position < wide.size(); position++) {
    auto code_point = static_cast<char32_t>(wide.at(position));
    // Only a 16-bit wchar_t holds the halves of a pair apart.
    if constexpr (sizeof(wchar_t) == 2) {
      if (IsHighSurrogate(code_point) && position + 1 < wide.size() &&
          IsLowSurrogate(static_cast<char32_t>(wide.at(position + 1)))) {
        code_point =
            supplementary_base +
            ((code_point - high_surrogate_first) << surrogate_payload_bits) +
            (static_cast<char32_t>(wide.at(position + 1)) -
             low_surrogate_first);
        position++;
      }
    }
    AppendUtf8(out, code_point);
  }

  return out;
}

std::wstring WideFromPath(const std::filesystem::path& path) {
#ifdef _WIN32
  return path.wstring();
#else
  return Utf8ToWide(path.string());
#endif
}

std::filesystem::path PathFromWide(std::wstring_view wide) {
#ifdef _WIN32
  return std::filesystem::path(std::wstring(wide));
#else
  return {WideToUtf8(wide)};
#endif
}

#ifndef _WIN32

std::wstring Utf8ToWide(std::string_view utf8) {
  std::wstring out;
  out.reserve(utf8.size());

  size_t position = 0;
  while (position < utf8.size()) {
    // position < utf8.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto lead = static_cast<unsigned char>(utf8[position]);
    char32_t code_point = 0;
    size_t length = 1;

    if ((lead & utf8_ascii_bit) == 0) {
      code_point = lead;
    } else if ((lead & utf8_lead2_mask) == utf8_lead2_tag) {
      code_point = lead & utf8_lead2_payload;
      length = 2;
    } else if ((lead & utf8_lead3_mask) == utf8_lead3_tag) {
      code_point = lead & utf8_lead3_payload;
      length = 3;
    } else if ((lead & utf8_lead4_mask) == utf8_lead4_tag) {
      code_point = lead & utf8_lead4_payload;
      length = 4;
    } else {
      position++;
      continue;
    }

    if (position + length > utf8.size()) {
      break;
    }

    bool valid = true;
    for (size_t k = 1; k < length; k++) {
      // position + length was checked against utf8.size() above.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const auto cont = static_cast<unsigned char>(utf8[position + k]);
      if ((cont & utf8_cont_mask) != utf8_cont_tag) {
        valid = false;
        break;
      }
      code_point = (code_point << utf8_cont_bits) | (cont & utf8_cont_payload);
    }

    if (!valid) {
      position++;
      continue;
    }

    out.push_back(gsl::narrow<wchar_t>(code_point));
    position += length;
  }

  return out;
}

#endif  // !_WIN32

}  // namespace NtfsDevice
