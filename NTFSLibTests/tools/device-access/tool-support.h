#pragma once

#include <cstdio>
#include <string_view>

#include <ntfs-browser/log/log.h>

#include "ntfs-browser/namespace.h"

namespace NtfsDevice {

// Reports whether arg is a "--log=" argument, in the wide argv a wmain() or
// a converted POSIX main() holds.
[[nodiscard]] bool IsLogOption(std::wstring_view arg);

// Parses one wide "--log=..." argument into config, like
// Log::ParseOption(). Returns false, leaving config untouched, when it is
// malformed.
[[nodiscard]] bool ParseLogOption(std::wstring_view arg,
                                  NTFS_BROWSER_NAMESPACE::Log::Config& config);

// Writes text to stream with no newline, as UTF-8 (see WideToUtf8()), whatever
// the locale or the code page: a name never stops at a character the code page
// lacks, and the output is the same on every machine. An unpaired surrogate is
// written in its WTF-8 form, and a U+0000 as a NUL byte.
void WriteText(std::FILE* stream, std::wstring_view text);

// Puts the console output code page of Windows in UTF-8 while it lives, so
// what the tools write as UTF-8 shows right in a console. It gives the code
// page back after. It does nothing when no console is attached, and off
// Windows.
class Utf8ConsoleOutput {
 public:
  Utf8ConsoleOutput();
  Utf8ConsoleOutput(const Utf8ConsoleOutput&) = delete;
  Utf8ConsoleOutput(Utf8ConsoleOutput&&) = delete;
  Utf8ConsoleOutput& operator=(const Utf8ConsoleOutput&) = delete;
  Utf8ConsoleOutput& operator=(Utf8ConsoleOutput&&) = delete;
  ~Utf8ConsoleOutput();

 private:
  // The code page to give back, or 0 when it was not changed.
  unsigned previous_code_page_{0};
};

// Tells stderr that the log file of config could not be opened.
void ReportLogFileError(const NTFS_BROWSER_NAMESPACE::Log::Config& config);

}  // namespace NtfsDevice
