#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace NtfsBrowser::Log
{

// Severity of a message, and, for a sink, the least severe message it
// accepts. Ordered most to least severe. kOff never matches a message, so
// as a sink threshold it silences that sink.
// The names carry a k prefix because <windows.h> defines ERROR and MFC
// defines TRACE as macros, which an ERROR or TRACE enumerator would hit.
// One byte wide: it is only ever a small tag.
enum class Level : std::uint8_t
{
  kOff,
  kError,
  kWarn,
  kInfo,
  kDebug,
  kTrace
};

// Prefix of the command-line argument ParseOption() accepts.
inline constexpr std::string_view kOptionPrefix = "--log=";

// One line of help for --log, for an executable's usage text.
inline constexpr std::string_view kOptionUsage =
    "--log=<console|file>:<off|error|warn|info|debug|trace>[:<path>]";

// Where the file sink writes when --log=file:<level> names no path.
inline constexpr std::string_view kDefaultFilePath = "ntfs-browser.log";

// Name of the logger the library emits through. It is deliberately not the
// process-wide default logger, which belongs to the host application.
inline constexpr std::string_view kLoggerName = "ntfs-browser";

// Runtime logging configuration, one level per target. The console target
// is split by severity: kError and kWarn go to stderr, kInfo and below go
// to stdout, so a message is printed to exactly one stream.
struct Config
{
  Level console_level = Level::kWarn;
  Level file_level = Level::kOff;
  // Narrow bytes, as the logging backend takes them: on Windows a path
  // outside the active ANSI code page cannot be opened, and the only
  // report of that is Configure() returning false.
  std::string file_path{kDefaultFilePath};
};

// Applies config, replacing the library logger's sinks and their levels
// wholesale. Returns false if the file sink could not be opened; the
// console target is installed either way. Never throws: the library logs
// from destructors and from noexcept functions.
// The library is not thread safe, and neither is logging: this call and
// every emitted message MUST come from one thread. The sinks are spdlog's
// single-threaded ones, so a message costs no lock.
bool Configure(const Config& config) noexcept;

// Parses one "--log=<target>:<level>[:<path>]" argument into config. Only
// the named target is touched, so the option may be repeated once per
// target. Splits on the first two colons only, so a Windows path keeps its
// drive letter. Returns false - leaving config untouched - if arg lacks
// kOptionPrefix, or names an unknown target or level.
bool ParseOption(std::string_view arg, Config& config) noexcept;

}  // namespace NtfsBrowser::Log
