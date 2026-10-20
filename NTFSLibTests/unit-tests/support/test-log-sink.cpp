#include "test-log-sink.h"

#include <memory>
#include <string>
#include <string_view>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {
namespace {

// Keeps every line the library logs, so a test can assert on text that
// otherwise only reaches a console. Redirecting a CRT file descriptor
// cannot do this: on Windows spdlog's console sinks cache the HANDLE that
// GetStdHandle() returned when they were built, so _dup2() on fd 1 leaves
// them writing to the real console and the capture comes back empty.
// No lock: the library and its tests log from one thread.
class CaptureSink final : public Log::ISink {
 public:
  [[nodiscard]] bool ShouldLog(Log::Level level) const override {
    return level != Log::Level::Off;
  }

  // The bare message plus a newline, so assertions match what the library
  // wrote.
  void Write(Log::Level /*level*/, std::string_view message) override {
    buffer_.append(message);
    buffer_.push_back('\n');
  }

  void Flush() override {}

  std::string Take() {
    std::string out;
    out.swap(buffer_);
    return out;
  }

 private:
  std::string buffer_;
};

std::shared_ptr<CaptureSink>& Sink() {
  static std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
  return sink;
}

// Runs before main(), so the very first test already logs at trace level.
[[maybe_unused]] const bool g_installed = [] {
  InstallCaptureSink();
  return true;
}();

}  // namespace

void InstallCaptureSink() { Log::SetSinks({Sink()}); }

std::shared_ptr<Log::ISink> CaptureSinkHandle() { return Sink(); }

std::string TakeCapturedLog() { return Sink()->Take(); }

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
