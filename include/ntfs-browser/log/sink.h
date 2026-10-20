#pragma once

#include <filesystem>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>

#include <ntfs-browser/export.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Log {

// A destination for the library's log messages: a file, a service, a
// test capture. The library offers each finished message to every
// installed sink. No logging backend appears here, so a consumer needs no
// particular library to write one.
// The library is single-threaded, and so is its logging: the library calls
// a sink from one thread only. A sink that is shared with other threads
// MUST take its own lock.
// Every member MAY throw. The library contains it: a throwing sink never
// makes a log call throw, and never keeps the next sink from its message.
// The library adds no queue: a slow or blocking sink delays the library
// call that logged. A sink that must not do so SHOULD hand the message to a
// thread of its own.
//
// A sink that forwards to a service:
//   class ServiceSink final : public Log::ISink {
//    public:
//     bool ShouldLog(Log::Level level) const override {
//       return level != Log::Level::Off && level <= Log::Level::Info;
//     }
//     void Write(Log::Level level, std::string_view message) override {
//       SendToService(level, message);
//     }
//     void Flush() override {}
//   };
//   Log::SetSinks({std::make_shared<ServiceSink>()});
class ISink {
 public:
  ISink() = default;
  ISink(ISink&&) = delete;
  ISink(const ISink&) = delete;
  ISink& operator=(ISink&&) = delete;
  ISink& operator=(const ISink&) = delete;
  virtual ~ISink() = default;

  // True if a message at this level is wanted. The library calls it before
  // it formats anything, so a sink that wants nothing costs one virtual
  // call per message.
  [[nodiscard]] virtual bool ShouldLog(Level level) const = 0;

  // Receives one finished message. It carries no timestamp, no level name
  // and no trailing newline: the sink adds what its destination needs.
  virtual void Write(Level level, std::string_view message) = 0;

  // Pushes whatever the sink buffered to its destination. The library
  // calls it after each message it wrote.
  virtual void Flush() = 0;
};

// Replaces the library's sinks with sinks, which receive every message from
// now on. Null entries are skipped. An empty set discards every message;
// Configure() with a default Config restores the stock console output.
// The library keeps a reference to each sink until the next call. A sink
// that depends on state which dies before the library's static objects
// MUST therefore be released with SetSinks({}) first.
// Never throws. Like Configure(), it MUST come from the one thread that
// logs. A sink MAY call it, or Configure(), from Write() or Flush(): the
// message in flight still reaches every sink of the set it was emitted to,
// and a sink that replaced itself stays alive until its call returns.
NTFS_BROWSER_EXPORT void
    SetSinks(std::span<const std::shared_ptr<ISink>> sinks) noexcept;

// SetSinks() for a braced list of sinks.
inline void
    SetSinks(std::initializer_list<std::shared_ptr<ISink>> sinks) noexcept {
  SetSinks(
      std::span<const std::shared_ptr<ISink>>(sinks.begin(), sinks.size()));
}

// The console target Configure() installs, as a sink: error and warn go to
// stderr, info and below to stdout, each message to exactly one of them.
// threshold is the least severe level it prints; Level::Off silences it.
NTFS_BROWSER_EXPORT std::shared_ptr<ISink> MakeConsoleSink(Level threshold);

// The file target Configure() installs, as a sink: one dated line per
// message, appended to path. Returns null if the file cannot be opened.
NTFS_BROWSER_EXPORT std::shared_ptr<ISink>
    MakeFileSink(const std::filesystem::path& path, Level threshold);

}  // namespace NTFS_BROWSER_NAMESPACE::Log
