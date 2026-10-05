#include "utf.h"

#include <cstddef>
#include <type_traits>

namespace NtfsBrowser
{
namespace
{

// First and last code unit of the UTF-16 high (leading) surrogate range.
constexpr char32_t high_surrogate_first = 0xD800;
constexpr char32_t high_surrogate_last = 0xDBFF;
// First and last code unit of the UTF-16 low (trailing) surrogate range.
constexpr char32_t low_surrogate_first = 0xDC00;
constexpr char32_t low_surrogate_last = 0xDFFF;
// Highest code point Unicode defines.
constexpr char32_t max_code_point = 0x10FFFF;
// Stands in for anything that is not a code point this decoder accepts.
constexpr char32_t replacement_character = 0xFFFD;
// Value a surrogate pair's combined 20-bit payload is offset by.
constexpr char32_t surrogate_base = 0x10000;
// Bits of the payload each half of a surrogate pair carries.
constexpr unsigned surrogate_shift = 10;
// Payload mask of one surrogate half, the low 10 bits.
constexpr char32_t surrogate_mask = 0x3FF;

// Upper bound, exclusive, of each UTF-8 encoding length.
constexpr char32_t one_byte_limit = 0x80;
constexpr char32_t two_byte_limit = 0x800;
constexpr char32_t three_byte_limit = 0x10000;

// Continuation bytes carry 6 payload bits each, tagged with 0b10.
constexpr unsigned continuation_shift = 6;
constexpr char32_t continuation_mask = 0x3F;
constexpr char32_t continuation_tag = 0x80;

// Tag bits of a lead byte, one per encoding length. Typed as char32_t so
// no operand of the byte arithmetic below is signed.
constexpr char32_t two_byte_tag = 0xC0;
constexpr char32_t three_byte_tag = 0xE0;
constexpr char32_t four_byte_tag = 0xF0;

// Appends cp's UTF-8 encoding to out.
void AppendUtf8(std::string& out, char32_t code_point)
{
  if (code_point < one_byte_limit)
  {
    out.push_back(static_cast<char>(code_point));
    return;
  }
  if (code_point < two_byte_limit)
  {
    out.push_back(
        static_cast<char>(two_byte_tag | (code_point >> continuation_shift)));
    out.push_back(
        static_cast<char>(continuation_tag | (code_point & continuation_mask)));
    return;
  }
  if (code_point < three_byte_limit)
  {
    out.push_back(static_cast<char>(three_byte_tag |
                                    (code_point >> (2 * continuation_shift))));
    out.push_back(static_cast<char>(
        continuation_tag |
        ((code_point >> continuation_shift) & continuation_mask)));
    out.push_back(
        static_cast<char>(continuation_tag | (code_point & continuation_mask)));
    return;
  }
  out.push_back(static_cast<char>(four_byte_tag |
                                  (code_point >> (3 * continuation_shift))));
  out.push_back(static_cast<char>(
      continuation_tag |
      ((code_point >> (2 * continuation_shift)) & continuation_mask)));
  out.push_back(static_cast<char>(
      continuation_tag |
      ((code_point >> continuation_shift) & continuation_mask)));
  out.push_back(
      static_cast<char>(continuation_tag | (code_point & continuation_mask)));
}

// True for a code unit that is one half of a surrogate pair.
bool IsSurrogate(char32_t unit) noexcept
{
  return unit >= high_surrogate_first && unit <= low_surrogate_last;
}

// Widens one code unit without sign-extending it: wchar_t is signed on
// some platforms, and a name's raw bytes may set the top bit.
template <class CharT>
char32_t Widen(CharT unit) noexcept
{
  return static_cast<char32_t>(static_cast<std::make_unsigned_t<CharT>>(unit));
}

// Shared decoder for both code-unit widths; see WideToUtf8()'s comment.
template <class CharT>
std::string ToUtf8(std::basic_string_view<CharT> units)
{
  std::string out;
  out.reserve(units.size());

  for (size_t i = 0; i < units.size(); ++i)
  {
    // i < units.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const char32_t unit = Widen(units[i]);

    if (unit >= high_surrogate_first && unit <= high_surrogate_last &&
        i + 1 < units.size())
    {
      // i + 1 < units.size() is part of the enclosing condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const char32_t low = Widen(units[i + 1]);
      if (low >= low_surrogate_first && low <= low_surrogate_last)
      {
        AppendUtf8(out, surrogate_base + (((unit - high_surrogate_first)
                                           << surrogate_shift) |
                                          (low & surrogate_mask)));
        ++i;
        continue;
      }
    }

    AppendUtf8(out, (IsSurrogate(unit) || unit > max_code_point)
                        ? replacement_character
                        : unit);
  }

  return out;
}

}  // namespace

std::string WideToUtf8(std::wstring_view wide) { return ToUtf8(wide); }

std::wstring Utf16ToWide(std::u16string_view units)
{
  std::wstring out;
  out.reserve(units.size());

  if constexpr (sizeof(wchar_t) > sizeof(char16_t))
  {
    for (size_t i = 0; i < units.size(); ++i)
    {
      // i < units.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const char32_t unit = Widen(units[i]);

      if (unit >= high_surrogate_first && unit <= high_surrogate_last &&
          i + 1 < units.size())
      {
        // i + 1 < units.size() is part of the enclosing condition.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const char32_t low = Widen(units[i + 1]);
        if (low >= low_surrogate_first && low <= low_surrogate_last)
        {
          out.push_back(static_cast<wchar_t>(
              surrogate_base +
              (((unit - high_surrogate_first) << surrogate_shift) |
               (low & surrogate_mask))));
          ++i;
          continue;
        }
      }
      out.push_back(static_cast<wchar_t>(unit));
    }
  }
  else
  {
    // wchar_t is exactly one code unit wide here: a surrogate pair stays as
    // two elements, matching how Windows itself represents one.
    for (const char16_t unit : units)
    {
      out.push_back(static_cast<wchar_t>(unit));
    }
  }

  return out;
}

}  // namespace NtfsBrowser
