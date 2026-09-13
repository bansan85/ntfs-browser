#pragma once

#ifdef _WIN32

  #ifndef NOMINMAX
    // Keeps windows.h from clobbering std::min/std::max.
    #define NOMINMAX
  #endif
  #include <windows.h>

#else

  // Minimal shim for the Windows typedefs this library's API uses.
  #include <cstddef>
  #include <cstdint>
  #include <cwctype>
  #include <type_traits>

using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using DWORD = std::uint32_t;
using LONG = std::int32_t;
using LONGLONG = std::int64_t;
using ULONGLONG = std::uint64_t;
using CHAR = char;

struct LARGE_INTEGER
{
  LONGLONG QuadPart;
};

struct FILETIME
{
  DWORD dwLowDateTime;
  DWORD dwHighDateTime;
};

// MSVC CRT function with no GCC/glibc equivalent; Filename::Compare is the
// only caller.
inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) noexcept
{
  for (size_t i = 0; i < n; ++i)
  {
    const wint_t ca = std::towupper(static_cast<wint_t>(a[i]));
    const wint_t cb = std::towupper(static_cast<wint_t>(b[i]));
    if (ca != cb)
    {
      return ca < cb ? -1 : 1;
    }
    if (a[i] == L'\0')
    {
      break;
    }
  }
  return 0;
}

// Reproduces <winnt.h>'s bitwise operators for a scoped enum, since
// Mask/Flag::* enums are used as OR/AND-able bitmasks throughout the library.
  #define DEFINE_ENUM_FLAG_OPERATORS(ENUMTYPE)                           \
    inline constexpr ENUMTYPE operator|(ENUMTYPE a, ENUMTYPE b) noexcept \
    {                                                                    \
      return static_cast<ENUMTYPE>(                                      \
          static_cast<std::underlying_type_t<ENUMTYPE>>(a) |             \
          static_cast<std::underlying_type_t<ENUMTYPE>>(b));             \
    }                                                                    \
    inline ENUMTYPE& operator|=(ENUMTYPE& a, ENUMTYPE b) noexcept        \
    {                                                                    \
      return a = a | b;                                                  \
    }                                                                    \
    inline constexpr ENUMTYPE operator&(ENUMTYPE a, ENUMTYPE b) noexcept \
    {                                                                    \
      return static_cast<ENUMTYPE>(                                      \
          static_cast<std::underlying_type_t<ENUMTYPE>>(a) &             \
          static_cast<std::underlying_type_t<ENUMTYPE>>(b));             \
    }                                                                    \
    inline ENUMTYPE& operator&=(ENUMTYPE& a, ENUMTYPE b) noexcept        \
    {                                                                    \
      return a = a & b;                                                  \
    }                                                                    \
    inline constexpr ENUMTYPE operator~(ENUMTYPE a) noexcept             \
    {                                                                    \
      return static_cast<ENUMTYPE>(                                      \
          ~static_cast<std::underlying_type_t<ENUMTYPE>>(a));            \
    }                                                                    \
    inline constexpr ENUMTYPE operator^(ENUMTYPE a, ENUMTYPE b) noexcept \
    {                                                                    \
      return static_cast<ENUMTYPE>(                                      \
          static_cast<std::underlying_type_t<ENUMTYPE>>(a) ^             \
          static_cast<std::underlying_type_t<ENUMTYPE>>(b));             \
    }                                                                    \
    inline ENUMTYPE& operator^=(ENUMTYPE& a, ENUMTYPE b) noexcept        \
    {                                                                    \
      return a = a ^ b;                                                  \
    }

#endif
