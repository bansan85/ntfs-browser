#ifndef _WIN32

  #include "linux-utf8.h"

  #include <cstddef>

namespace NtfsCompare
{

std::wstring Utf8ToWide(std::string_view utf8)
{
  std::wstring out;
  out.reserve(utf8.size());

  size_t i = 0;
  while (i < utf8.size())
  {
    const auto lead = static_cast<unsigned char>(utf8[i]);
    char32_t codePoint = 0;
    size_t length = 1;

    if ((lead & 0x80U) == 0)
    {
      codePoint = lead;
    }
    else if ((lead & 0xE0U) == 0xC0U)
    {
      codePoint = lead & 0x1FU;
      length = 2;
    }
    else if ((lead & 0xF0U) == 0xE0U)
    {
      codePoint = lead & 0x0FU;
      length = 3;
    }
    else if ((lead & 0xF8U) == 0xF0U)
    {
      codePoint = lead & 0x07U;
      length = 4;
    }
    else
    {
      i++;
      continue;
    }

    if (i + length > utf8.size())
    {
      break;
    }

    bool valid = true;
    for (size_t k = 1; k < length; k++)
    {
      const auto cont = static_cast<unsigned char>(utf8[i + k]);
      if ((cont & 0xC0U) != 0x80U)
      {
        valid = false;
        break;
      }
      codePoint = (codePoint << 6U) | (cont & 0x3FU);
    }

    if (!valid)
    {
      i++;
      continue;
    }

    out.push_back(static_cast<wchar_t>(codePoint));
    i += length;
  }

  return out;
}

}  // namespace NtfsCompare

#endif  // !_WIN32
