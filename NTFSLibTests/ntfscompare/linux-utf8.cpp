#ifndef _WIN32

  #include "linux-utf8.h"

  #include <cstddef>

  #include <gsl/narrow>

namespace NtfsCompare
{

namespace
{
// UTF-8 lead byte of a 1-byte sequence: bit 7 clear.
constexpr unsigned utf8_ascii_bit = 0x80U;

// UTF-8 lead bytes, per sequence length: the bits that identify the length
// (mask), the pattern they hold (tag), and the bits left for the code point.
constexpr unsigned utf8_lead2_mask = 0xE0U;
constexpr unsigned utf8_lead2_tag = 0xC0U;
constexpr unsigned utf8_lead2_payload = 0x1FU;
constexpr unsigned utf8_lead3_mask = 0xF0U;
constexpr unsigned utf8_lead3_tag = 0xE0U;
constexpr unsigned utf8_lead3_payload = 0x0FU;
constexpr unsigned utf8_lead4_mask = 0xF8U;
constexpr unsigned utf8_lead4_tag = 0xF0U;
constexpr unsigned utf8_lead4_payload = 0x07U;

// UTF-8 continuation byte: the bits that identify it (mask), the pattern
// they hold (tag), the bits left for the code point, and how many there are.
constexpr unsigned utf8_cont_mask = 0xC0U;
constexpr unsigned utf8_cont_tag = 0x80U;
constexpr unsigned utf8_cont_payload = 0x3FU;
constexpr unsigned utf8_cont_bits = 6U;
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
    char32_t code_point = 0;
    size_t length = 1;

    if ((lead & utf8_ascii_bit) == 0)
    {
      code_point = lead;
    }
    else if ((lead & utf8_lead2_mask) == utf8_lead2_tag)
    {
      code_point = lead & utf8_lead2_payload;
      length = 2;
    }
    else if ((lead & utf8_lead3_mask) == utf8_lead3_tag)
    {
      code_point = lead & utf8_lead3_payload;
      length = 3;
    }
    else if ((lead & utf8_lead4_mask) == utf8_lead4_tag)
    {
      code_point = lead & utf8_lead4_payload;
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
      if ((cont & utf8_cont_mask) != utf8_cont_tag)
      {
        valid = false;
        break;
      }
      code_point = (code_point << utf8_cont_bits) | (cont & utf8_cont_payload);
    }

    if (!valid)
    {
      position++;
      continue;
    }

    out.push_back(gsl::narrow<wchar_t>(code_point));
    position += length;
  }

  return out;
}

}  // namespace NtfsCompare

#endif  // !_WIN32
