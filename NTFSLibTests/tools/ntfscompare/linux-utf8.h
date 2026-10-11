#pragma once

#ifndef _WIN32

  #include <string>
  #include <string_view>

namespace NtfsCompare {

// Decodes a UTF-8 byte sequence (Linux filenames' usual encoding) into a
// wide string of Unicode code points, so it can share this tool's wstring
// path keys with the wide-native Windows side and the library's own decoded
// names. An invalid lead or continuation byte is skipped rather than
// aborting: a best-effort decode is enough for a comparison report.
[[nodiscard]] std::wstring Utf8ToWide(std::string_view utf8);

}  // namespace NtfsCompare

#endif  // !_WIN32
