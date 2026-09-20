#pragma once

#include <string>
#include <string_view>

namespace NtfsBrowser
{

// Converts an on-disk name to UTF-8, for logging. An unpaired surrogate
// becomes U+FFFD, so the result is always well-formed UTF-8.
// wchar_t is 16 bits on Windows and 32 bits elsewhere, and one decoder
// handles both widths: a unit above 0xFFFF can only be a whole code point,
// never half of a surrogate pair.
std::string WideToUtf8(std::wstring_view wide);

}  // namespace NtfsBrowser
