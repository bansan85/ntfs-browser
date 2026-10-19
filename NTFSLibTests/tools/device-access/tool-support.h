#pragma once

#include <cstdio>
#include <string_view>

#include <ntfs-browser/log/log.h>

namespace NtfsDevice {

// Reports whether arg is a "--log=" argument, in the wide argv a wmain() or
// a converted POSIX main() holds.
[[nodiscard]] bool IsLogOption(std::wstring_view arg);

// Parses one wide "--log=..." argument into config, like
// Log::ParseOption(). Returns false, leaving config untouched, when it is
// malformed.
[[nodiscard]] bool ParseLogOption(std::wstring_view arg,
                                  NTFS_BROWSER_NAMESPACE::Log::Config& config);

// Writes text to stream with no newline. Windows prints it through the CRT's
// wide conversion, as the Windows samples always did. Elsewhere it is UTF-8,
// whatever the locale, so the output is the same on every machine.
void WriteText(std::FILE* stream, std::wstring_view text);

// Tells stderr that the log file of config could not be opened.
void ReportLogFileError(const NTFS_BROWSER_NAMESPACE::Log::Config& config);

}  // namespace NtfsDevice
