#pragma once

#include <string>
#include <string_view>

#include "internal-export.h"

namespace NtfsBrowser
{

// Converts an on-disk name to UTF-8, for logging. An unpaired surrogate
// becomes U+FFFD, so the result is always well-formed UTF-8.
// wchar_t is 16 bits on Windows and 32 bits elsewhere, and one decoder
// handles both widths: a unit above 0xFFFF can only be a whole code point,
// never half of a surrogate pair.
NTFS_BROWSER_EXPORT_TESTS_ONLY std::string WideToUtf8(std::wstring_view wide);

// Decodes units - an on-disk name's raw UTF-16 code units - into wchar_t.
// On Windows, wchar_t already is a 16-bit UTF-16 code unit, so this is a
// plain widen. Elsewhere (eg. Linux, where wchar_t is 32 bits) a valid
// surrogate pair is merged into the single wchar_t it encodes; a code unit
// that is not part of one passes through as its own numeric value.
NTFS_BROWSER_EXPORT_TESTS_ONLY std::wstring
    Utf16ToWide(std::u16string_view units);

}  // namespace NtfsBrowser
