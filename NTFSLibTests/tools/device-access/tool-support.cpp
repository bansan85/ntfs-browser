#include "tool-support.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>

#include "text-convert.h"

namespace Log = NTFS_BROWSER_NAMESPACE::Log;

namespace NtfsDevice {

bool IsLogOption(std::wstring_view arg) {
#ifdef _WIN32
  return arg.starts_with(Log::option_prefix_w);
#else
  return WideToUtf8(arg).starts_with(Log::option_prefix);
#endif
}

bool ParseLogOption(std::wstring_view arg, Log::Config& config) {
#ifdef _WIN32
  return Log::ParseOption(arg, config);
#else
  return Log::ParseOption(WideToUtf8(arg), config);
#endif
}

void WriteText(std::FILE* stream, std::wstring_view text) {
  const std::string utf8 = WideToUtf8(text);
  // A console stream: nothing can be done about a failed write.
  (void)fwrite(utf8.data(), 1, utf8.size(), stream);
}

#ifdef _WIN32

Utf8ConsoleOutput::Utf8ConsoleOutput() {
  // 0 when no console is attached: nothing to switch.
  const UINT current = GetConsoleOutputCP();
  if (current != 0 && current != CP_UTF8 && SetConsoleOutputCP(CP_UTF8) != 0) {
    previous_code_page_ = current;
  }
}

Utf8ConsoleOutput::~Utf8ConsoleOutput() {
  if (previous_code_page_ != 0) {
    SetConsoleOutputCP(previous_code_page_);
  }
}

#else

Utf8ConsoleOutput::Utf8ConsoleOutput() = default;

Utf8ConsoleOutput::~Utf8ConsoleOutput() = default;

#endif

void ReportLogFileError(const Log::Config& config) {
  // Nothing can be done about a failed write to stderr.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,cert-err33-c)
#ifdef _WIN32
  fprintf(stderr, "Cannot open log file %s\n",
          WideToUtf8(config.file_path.native()).c_str());
#else
  fprintf(stderr, "Cannot open log file %s\n", config.file_path.c_str());
#endif
  // NOLINTEND(cppcoreguidelines-pro-type-vararg,cert-err33-c)
}

}  // namespace NtfsDevice
