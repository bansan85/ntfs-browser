#pragma once

// Conversions between the wide strings of the command line and the UTF-16
// text of Scan. wchar_t is UTF-16 on Windows and UTF-32 elsewhere.

#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir {

// The UTF-16 units of wide. A code point past U+FFFF becomes a surrogate pair
// and a value past U+10FFFF becomes U+FFFD. A lone surrogate passes through.
[[nodiscard]] std::u16string ToUtf16(std::wstring_view wide);

// The wide string of units. A valid surrogate pair becomes one code point
// where wchar_t is 32 bits. A lone surrogate passes through as its own value.
[[nodiscard]] std::wstring FromUtf16(std::u16string_view units);

}  // namespace NTFS_BROWSER_NAMESPACE::Example::NtfsDir
