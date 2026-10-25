#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>

#include "internal-export.h"

namespace NTFS_BROWSER_NAMESPACE::Log::Detail {

// True while at least one configured sink accepts this level. Call sites
// test it before formatting, so a disabled level costs one out-of-line
// call - across the DLL boundary under BUILD_SHARED_LIBS=ON - rather than
// a std::format() call and the string it allocates.
NTFS_BROWSER_EXPORT_TESTS_ONLY bool IsEnabled(Level level) noexcept;

// Mutes the calling thread for good: IsEnabled() is false there and Emit()
// drops its line. A worker thread of the library calls it first, since the
// sinks are single-threaded and every message MUST come from the caller's
// thread.
NTFS_BROWSER_EXPORT_TESTS_ONLY void SilenceCurrentThread() noexcept;

// Hands one finished line to the library logger. On a thread that captures
// (see CaptureScope), the line goes to the capture instead of the sinks.
NTFS_BROWSER_EXPORT_TESTS_ONLY void Emit(Level level,
                                         std::string_view message) noexcept;

// One bit per level, to tell which levels a capture keeps. Bit n is set for the
// level whose value is n.
using LevelMask = std::uint32_t;

// The bit of level in a LevelMask.
[[nodiscard]] constexpr LevelMask LevelBit(Level level) noexcept {
  return LevelMask{1} << static_cast<unsigned>(level);
}

// The levels IsEnabled() accepts right now, on the calling thread. The library
// thread that owns the sinks takes it before it starts a worker that captures:
// a worker MUST NOT ask the sinks itself.
NTFS_BROWSER_EXPORT_TESTS_ONLY LevelMask EnabledLevels() noexcept;

// The lines a worker thread logged, kept apart from the sinks. The sinks are
// single-threaded, so a worker of the library captures its messages here and
// the thread that owns the sinks replays them, in the order of the work they
// belong to. A capture is read by one thread after the capturing thread is
// done with it, and the caller MUST order the two (a lock, a join).
class NTFS_BROWSER_EXPORT_TESTS_ONLY CapturedLog {
 public:
  // A place in the log: the lines held so far, and how many salvaged conditions
  // (RecoveredCount()) the capturing thread had counted by then.
  struct Mark {
    size_t lines{0};
    std::uint64_t recovered{0};
  };

  // Forgets every line, keeps the memory, and keeps only the levels in levels
  // from now on.
  void Reset(LevelMask levels) noexcept;

  // The levels this capture keeps.
  [[nodiscard]] LevelMask Levels() const noexcept { return levels_; }

  // Adds one line. A line that cannot be stored is dropped: the capture
  // belongs to logging, which never throws.
  void Append(Level level, std::string_view message) noexcept;

  // The end of the log, as the capturing thread sees it.
  [[nodiscard]] Mark End() const noexcept;

  // Sends the lines between two marks to the sinks, in order, and adds the
  // salvaged conditions counted between them to the calling thread's count.
  // The calling thread MUST be the one that owns the sinks, and MUST NOT be
  // capturing.
  void Replay(const Mark& from, const Mark& to) const noexcept;

 private:
  // A line held in text_: its level, and where it ends.
  struct Line {
    Level level;
    size_t end;
  };

  std::string text_;
  std::vector<Line> lines_;
  LevelMask levels_{0};
};

// While one lives, the calling thread captures: IsEnabled() answers from the
// levels of log, and Emit() appends to it. The sinks are not touched.
class NTFS_BROWSER_EXPORT_TESTS_ONLY CaptureScope {
 public:
  explicit CaptureScope(CapturedLog& log) noexcept;
  CaptureScope(const CaptureScope&) = delete;
  CaptureScope& operator=(const CaptureScope&) = delete;
  CaptureScope(CaptureScope&&) = delete;
  CaptureScope& operator=(CaptureScope&&) = delete;
  ~CaptureScope();

 private:
  CapturedLog* previous_;
};

// Counts one salvaged condition on the calling thread. Called by
// Log::Recoverable() when its caller recovers, whether or not a sink wants the
// message.
NTFS_BROWSER_EXPORT_TESTS_ONLY void NoteRecovered() noexcept;

// How many conditions the calling thread has salvaged so far. A caller that
// needs to know whether a stretch of work salvaged anything compares the value
// before and after it.
NTFS_BROWSER_EXPORT_TESTS_ONLY std::uint64_t RecoveredCount() noexcept;

// Takes back the salvaged conditions the calling thread counted since mark, as
// End() saw them. A capture that runs on the thread that replays it counts each
// condition twice, once as it is met and once at the replay: the caller
// rewinds after the capture, and the replay counts them where they belong.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    ForgetRecoveredSince(const CapturedLog::Mark& mark) noexcept;

// Formats and emits. A formatting or allocation failure yields a stand-in
// line instead of a throw: callers include destructors and noexcept
// functions, where a throw would end the process.
template <class... Args>
void Write(Level level, std::format_string<Args...> fmt,
           Args&&... args) noexcept {
  if (!IsEnabled(level)) {
    return;
  }

  try {
    Emit(level, std::format(fmt, std::forward<Args>(args)...));
  } catch (...) {
    // One line lost beats a throw out of a destructor, but say so.
    Emit(level, "Log message could not be formatted");
  }
}

}  // namespace NTFS_BROWSER_NAMESPACE::Log::Detail

namespace NTFS_BROWSER_NAMESPACE::Log {

// The library's logging entry points. Every diagnostic goes through one of
// them, which keeps the logging backend out of every other translation
// unit. The first argument is always a literal format string; runtime text
// is passed as an argument, since it may contain braces.

template <class... Args>
void Trace(std::format_string<Args...> fmt, Args&&... args) noexcept {
  Detail::Write(Level::Trace, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void Debug(std::format_string<Args...> fmt, Args&&... args) noexcept {
  Detail::Write(Level::Debug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) noexcept {
  Detail::Write(Level::Info, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) noexcept {
  Detail::Write(Level::Warn, fmt, std::forward<Args>(args)...);
}

// A salvageable condition: Info when recover is true (recover_errors is on,
// so the caller salvages), Warn when strict (the caller rejects the item
// whole). The text is identical either way. A call made with recover set is
// also counted (Detail::RecoveredCount()), so that a record can tell it was
// salvaged. Only a condition that strict mode rejects belongs here.
template <class... Args>
void Recoverable(bool recover, std::format_string<Args...> fmt,
                 Args&&... args) noexcept {
  if (recover) {
    Detail::NoteRecovered();
  }
  Detail::Write(recover ? Level::Info : Level::Warn, fmt,
                std::forward<Args>(args)...);
}

template <class... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) noexcept {
  Detail::Write(Level::Error, fmt, std::forward<Args>(args)...);
}

// True while a message at this level would reach a sink. A call site
// tests it only when building an argument costs something on its own,
// such as converting an on-disk UTF-16 name to UTF-8.
inline bool IsLogged(Level level) noexcept { return Detail::IsEnabled(level); }

// Relays a caught exception at error level. what() is caller data, so it
// is never a format string; the trailing newline some throw sites write
// is dropped, so one exception still yields one line.
NTFS_BROWSER_EXPORT_TESTS_ONLY void
    Exception(const std::exception& exception) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Log
