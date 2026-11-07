#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace NtfsDevice {

// Encodes a wide string as UTF-8. wchar_t is UTF-16 on Windows and UTF-32
// elsewhere, and both are handled. A valid surrogate pair becomes one 4-byte
// sequence. An unpaired surrogate becomes its own 3-byte form (WTF-8), the
// way the Linux NTFS drivers show a name that has no UTF-8 form: U+D800 is
// ED A0 80. A value above U+10FFFF becomes U+FFFD.
[[nodiscard]] std::string WideToUtf8(std::wstring_view wide);

// Appends wide to out, encoded as WideToUtf8() does.
void AppendAsUtf8(std::string& out, std::wstring_view wide);

// Converts a path to the wide string the tools use as a path key. UTF-8 on
// POSIX, the native wide form on Windows.
[[nodiscard]] std::wstring WideFromPath(const std::filesystem::path& path);

// The reverse of WideFromPath().
[[nodiscard]] std::filesystem::path PathFromWide(std::wstring_view wide);

#ifndef _WIN32

// Decodes a UTF-8 byte sequence (Linux filenames' usual encoding) into a
// wide string of Unicode code points, so it can share this tool's wstring
// path keys with the wide-native Windows side and the library's own decoded
// names. The three-byte form of a surrogate (WTF-8) decodes to that
// surrogate's code unit, so WideToUtf8() round-trips. An invalid lead or
// continuation byte is skipped rather than aborting: a best-effort decode is
// enough for a comparison report.
[[nodiscard]] std::wstring Utf8ToWide(std::string_view utf8);

#endif  // !_WIN32

}  // namespace NtfsDevice
