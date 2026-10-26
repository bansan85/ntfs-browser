#pragma once

#include <string>
#include <string_view>

#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Utf {

// Converts an on-disk name to UTF-8, for logging. A valid surrogate pair
// becomes one 4-byte sequence. An unpaired surrogate becomes its own 3-byte
// form (WTF-8: U+D800 is ED A0 80), which NTFS stores as it is, so two names
// that differ only by one stay distinct. That is not well-formed UTF-8. A
// value above U+10FFFF becomes U+FFFD. The tools' own conversion
// (NtfsDevice::WideToUtf8) follows the same rule.
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

}  // namespace NTFS_BROWSER_NAMESPACE::Utf
