#ifndef _WIN32

  #include "linux-utf8.h"

  #include <cstddef>

  #include <gsl/narrow>

namespace NtfsCompare
{

namespace
{
// UTF-8 lead byte of a 1-byte sequence: bit 7 clear.
constexpr unsigned kUtf8AsciiBit = 0x80U;

// UTF-8 lead bytes, per sequence length: the bits that identify the length
// (mask), the pattern they hold (tag), and the bits left for the code point.
constexpr unsigned kUtf8Lead2Mask = 0xE0U;
constexpr unsigned kUtf8Lead2Tag = 0xC0U;
constexpr unsigned kUtf8Lead2Payload = 0x1FU;
constexpr unsigned kUtf8Lead3Mask = 0xF0U;
constexpr unsigned kUtf8Lead3Tag = 0xE0U;
constexpr unsigned kUtf8Lead3Payload = 0x0FU;
constexpr unsigned kUtf8Lead4Mask = 0xF8U;
constexpr unsigned kUtf8Lead4Tag = 0xF0U;
constexpr unsigned kUtf8Lead4Payload = 0x07U;

// UTF-8 continuation byte: the bits that identify it (mask), the pattern
// they hold (tag), the bits left for the code point, and how many there are.
constexpr unsigned kUtf8ContMask = 0xC0U;
constexpr unsigned kUtf8ContTag = 0x80U;
constexpr unsigned kUtf8ContPayload = 0x3FU;
constexpr unsigned kUtf8ContBits = 6U;
}  // namespace

std::wstring Utf8ToWide(std::string_view utf8)
{
  std::wstring out;
  out.reserve(utf8.size());

  size_t position = 0;
  while (position < utf8.size())
  {
    // position < utf8.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const auto lead = static_cast<unsigned char>(utf8[position]);
    char32_t codePoint = 0;
    size_t length = 1;

    if ((lead & kUtf8AsciiBit) == 0)
    {
      codePoint = lead;
    }
    else if ((lead & kUtf8Lead2Mask) == kUtf8Lead2Tag)
    {
      codePoint = lead & kUtf8Lead2Payload;
      length = 2;
    }
    else if ((lead & kUtf8Lead3Mask) == kUtf8Lead3Tag)
    {
      codePoint = lead & kUtf8Lead3Payload;
      length = 3;
    }
    else if ((lead & kUtf8Lead4Mask) == kUtf8Lead4Tag)
    {
      codePoint = lead & kUtf8Lead4Payload;
      length = 4;
    }
    else
    {
      position++;
      continue;
    }

    if (position + length > utf8.size())
    {
      break;
    }

    bool valid = true;
    for (size_t k = 1; k < length; k++)
    {
      // position + length was checked against utf8.size() above.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      const auto cont = static_cast<unsigned char>(utf8[position + k]);
      if ((cont & kUtf8ContMask) != kUtf8ContTag)
      {
        valid = false;
        break;
      }
      codePoint = (codePoint << kUtf8ContBits) | (cont & kUtf8ContPayload);
    }

    if (!valid)
    {
      position++;
      continue;
    }

    out.push_back(gsl::narrow<wchar_t>(codePoint));
    position += length;
  }

  return out;
}

}  // namespace NtfsCompare

#endif  // !_WIN32
