#include "test-log-sink.h"

#include <memory>
#include <string>

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/log.h>

namespace NtfsBrowserTests
{
namespace
{

// Keeps every line the library logs, so a test can assert on text that
// otherwise only reaches a console. Redirecting a CRT file descriptor
// cannot do this: on Windows spdlog's console sinks cache the HANDLE that
// GetStdHandle() returned when they were built, so _dup2() on fd 1 leaves
// them writing to the real console and the capture comes back empty.
// Null mutex: the library and its tests log from one thread, matching
// the single-threaded sinks Configure() installs.
class CaptureSink final
    : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
{
 public:
  std::string Take()
  {
    std::string out;
    out.swap(buffer_);
    return out;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override
  {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    buffer_.append(formatted.data(), formatted.size());
  }

  void flush_() override {}

 private:
  std::string buffer_;
};

std::shared_ptr<CaptureSink>& Sink()
{
  static std::shared_ptr<CaptureSink> sink = std::make_shared<CaptureSink>();
  return sink;
}

// Runs before main(), so the very first test already logs at trace level.
[[maybe_unused]] const bool g_installed = []
{
  InstallCaptureSink();
  return true;
}();

}  // namespace

void InstallCaptureSink()
{
  NtfsBrowser::Log::Config config;
  config.console_level = NtfsBrowser::Log::Level::kOff;
  config.file_level = NtfsBrowser::Log::Level::kOff;
  NtfsBrowser::Log::Configure(config);

  const std::shared_ptr<spdlog::logger> logger =
      spdlog::get(std::string(NtfsBrowser::Log::kLoggerName));
  if (!logger)
  {
    return;
  }

  const std::shared_ptr<CaptureSink>& sink = Sink();
  sink->set_level(spdlog::level::trace);
  // The bare message, so assertions match what the library wrote.
  sink->set_pattern("%v");
  logger->sinks().push_back(sink);
  logger->set_level(spdlog::level::trace);
}

std::string TakeCapturedLog() { return Sink()->Take(); }

}  // namespace NtfsBrowserTests
