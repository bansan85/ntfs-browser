#pragma once

#include <exception>
#include <format>
#include <string_view>
#include <utility>

#include <ntfs-browser/log.h>

namespace NtfsBrowser::Log::Detail
{

// True while at least one configured sink accepts this level. Call sites
// test it before formatting, so a disabled level costs one out-of-line
// call - across the DLL boundary under BUILD_SHARED_LIBS=ON - rather than
// a std::format() call and the string it allocates.
bool IsEnabled(Level level) noexcept;

// Hands one finished line to the library logger.
void Emit(Level level, std::string_view message) noexcept;

// Formats and emits. A formatting or allocation failure yields a stand-in
// line instead of a throw: callers include destructors and noexcept
// functions, where a throw would end the process.
template <class... Args>
void Write(Level level, std::format_string<Args...> fmt,
           Args&&... args) noexcept
{
  if (!IsEnabled(level))
  {
    return;
  }

  try
  {
    Emit(level, std::format(fmt, std::forward<Args>(args)...));
  }
  catch (...)
  {
    // One line lost beats a throw out of a destructor, but say so.
    Emit(level, "Log message could not be formatted");
  }
}

}  // namespace NtfsBrowser::Log::Detail

namespace NtfsBrowser
{

// The library's logging entry points. Every diagnostic goes through one of
// them, which keeps the logging backend out of every other translation
// unit. The first argument is always a literal format string; runtime text
// is passed as an argument, since it may contain braces.

template <class... Args>
void LogTrace(std::format_string<Args...> fmt, Args&&... args) noexcept
{
  Log::Detail::Write(Log::Level::kTrace, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void LogDebug(std::format_string<Args...> fmt, Args&&... args) noexcept
{
  Log::Detail::Write(Log::Level::kDebug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void LogInfo(std::format_string<Args...> fmt, Args&&... args) noexcept
{
  Log::Detail::Write(Log::Level::kInfo, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void LogWarn(std::format_string<Args...> fmt, Args&&... args) noexcept
{
  Log::Detail::Write(Log::Level::kWarn, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void LogError(std::format_string<Args...> fmt, Args&&... args) noexcept
{
  Log::Detail::Write(Log::Level::kError, fmt, std::forward<Args>(args)...);
}

// True while a message at this level would reach a sink. A call site
// tests it only when building an argument costs something on its own,
// such as converting an on-disk UTF-16 name to UTF-8.
inline bool IsLogged(Log::Level level) noexcept
{
  return Log::Detail::IsEnabled(level);
}

// Relays a caught exception at error level. what() is caller data, so it
// is never a format string; the trailing newline some throw sites write
// is dropped, so one exception still yields one line.
void LogException(const std::exception& e) noexcept;

}  // namespace NtfsBrowser
