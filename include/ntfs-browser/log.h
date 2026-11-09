#pragma once

// The log of the library. Scan and Reader report what they
// cannot do (a target that does not open, a damaged folder, an unreadable
// record) as log lines, never as exceptions. This header is the one way to
// receive them or to silence them. It needs nothing but the standard library.
//
//   SetLogSink(
//       [](LogLevel level, std::string_view message) {
//         MyLog(static_cast<int>(level), message);
//       },
//       LogLevel::Info);
//
// Until the first call, the library writes its warnings and errors to the
// console (stderr). SetLogSink({}, LogLevel::Off) discards
// every line.
//
// Threads. The library and its log are not thread safe. SetLogSink() and every
// call of the sink come from one thread: the thread that polls the Scan or
// calls the Reader. The worker thread of a Scan never calls the sink: Poll()
// replays what the worker logged. Call SetLogSink() from that thread too, while
// no Scan runs.

#include <cstdint>
#include <functional>
#include <string_view>

#include <ntfs-browser/export.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE {

// Severity of a line, and, for the sink, the least severe line it accepts.
// Ordered from the most to the least severe. Off never matches a line, so as a
// threshold it silences the sink. One byte wide: it is only a small tag.
enum class LogLevel : std::uint8_t { Off, Error, Warn, Info, Debug, Trace };

// Receives one finished line: UTF-8 text without a time stamp, without a level
// name and without a trailing newline. The sink adds what its destination
// needs. It MAY throw: the library catches it and goes on.
using LogSink = std::function<void(LogLevel level, std::string_view message)>;

// Replaces the log of the library with sink, which receives every line whose
// level is at most threshold (Error is the most severe, so Warn gets the errors
// and the warnings). An empty sink discards every line. The previous sink is
// released before the call returns. May throw std::bad_alloc.
NTFS_BROWSER_EXPORT void SetLogSink(LogSink sink, LogLevel threshold);

}  // namespace NTFS_BROWSER_NAMESPACE
