#include "utf.h"

#include <cstddef>
#include <type_traits>

namespace NtfsBrowser
{
namespace
{

// First and last code unit of the UTF-16 high (leading) surrogate range.
constexpr char32_t kHighSurrogateFirst = 0xD800;
constexpr char32_t kHighSurrogateLast = 0xDBFF;
// First and last code unit of the UTF-16 low (trailing) surrogate range.
constexpr char32_t kLowSurrogateFirst = 0xDC00;
constexpr char32_t kLowSurrogateLast = 0xDFFF;
// Highest code point Unicode defines.
constexpr char32_t kMaxCodePoint = 0x10FFFF;
// Stands in for anything that is not a code point this decoder accepts.
constexpr char32_t kReplacementCharacter = 0xFFFD;
// Value a surrogate pair's combined 20-bit payload is offset by.
constexpr char32_t kSurrogateBase = 0x10000;
// Bits of the payload each half of a surrogate pair carries.
constexpr unsigned kSurrogateShift = 10;
// Payload mask of one surrogate half, the low 10 bits.
constexpr char32_t kSurrogateMask = 0x3FF;

// Upper bound, exclusive, of each UTF-8 encoding length.
constexpr char32_t kOneByteLimit = 0x80;
constexpr char32_t kTwoByteLimit = 0x800;
constexpr char32_t kThreeByteLimit = 0x10000;

// Continuation bytes carry 6 payload bits each, tagged with 0b10.
constexpr unsigned kContinuationShift = 6;
constexpr char32_t kContinuationMask = 0x3F;
constexpr char32_t kContinuationTag = 0x80;

// Tag bits of a lead byte, one per encoding length. Typed as char32_t so
// no operand of the byte arithmetic below is signed.
constexpr char32_t kTwoByteTag = 0xC0;
constexpr char32_t kThreeByteTag = 0xE0;
constexpr char32_t kFourByteTag = 0xF0;

// Appends cp's UTF-8 encoding to out.
void AppendUtf8(std::string& out, char32_t cp)
{
  if (cp < kOneByteLimit)
  {
    out.push_back(static_cast<char>(cp));
    return;
  }
  if (cp < kTwoByteLimit)
  {
    out.push_back(static_cast<char>(kTwoByteTag | (cp >> kContinuationShift)));
    out.push_back(
        static_cast<char>(kContinuationTag | (cp & kContinuationMask)));
    return;
  }
  if (cp < kThreeByteLimit)
  {
    out.push_back(
        static_cast<char>(kThreeByteTag | (cp >> (2 * kContinuationShift))));
    out.push_back(static_cast<char>(
        kContinuationTag | ((cp >> kContinuationShift) & kContinuationMask)));
    out.push_back(
        static_cast<char>(kContinuationTag | (cp & kContinuationMask)));
    return;
  }
  out.push_back(
      static_cast<char>(kFourByteTag | (cp >> (3 * kContinuationShift))));
  out.push_back(static_cast<char>(
      kContinuationTag |
      ((cp >> (2 * kContinuationShift)) & kContinuationMask)));
  out.push_back(static_cast<char>(
      kContinuationTag | ((cp >> kContinuationShift) & kContinuationMask)));
  out.push_back(static_cast<char>(kContinuationTag | (cp & kContinuationMask)));
}

// True for a code unit that is one half of a surrogate pair.
bool IsSurrogate(char32_t unit) noexcept
{
  return unit >= kHighSurrogateFirst && unit <= kLowSurrogateLast;
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
    const char32_t unit = Widen(units[i]);

    if (unit >= kHighSurrogateFirst && unit <= kHighSurrogateLast &&
        i + 1 < units.size())
    {
      const char32_t low = Widen(units[i + 1]);
      if (low >= kLowSurrogateFirst && low <= kLowSurrogateLast)
      {
        AppendUtf8(out, kSurrogateBase +
                            (((unit - kHighSurrogateFirst) << kSurrogateShift) |
                             (low & kSurrogateMask)));
        ++i;
        continue;
      }
    }

    AppendUtf8(out, (IsSurrogate(unit) || unit > kMaxCodePoint)
                        ? kReplacementCharacter
                        : unit);
  }

  return out;
}

}  // namespace

std::string WideToUtf8(std::wstring_view wide) { return ToUtf8(wide); }

}  // namespace NtfsBrowser
