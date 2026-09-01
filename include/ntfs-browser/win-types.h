#pragma once

// Basic Windows types the on-disk struct layouts and public API are
// expressed in. On Windows this is just <windows.h> (byte-identical
// behavior); elsewhere it's a minimal, additive shim covering exactly the
// symbols this library uses, so the library and its non-Win32-API-calling
// consumers (eg. NtfsVolume built from an already-open IDiskReader) can be
// compiled with a non-MSVC toolchain. Actual Win32 API calls (CreateFileW,
// ReadFile, ...) stay confined to win32-disk-reader.{h,cpp}, which is not
// compiled outside Windows.

#ifdef _WIN32

  #ifndef NOMINMAX
    #define NOMINMAX  // keep windows.h from clobbering std::min/std::max
  #endif
  #include <windows.h>

#else

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

// Reproduces the bitwise operator overloads <winnt.h> defines for scoped
// enums via DEFINE_ENUM_FLAG_OPERATORS, since Mask/Flag::* enums rely on
// them and are used as plain OR/AND-able bitmasks throughout the library.
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
