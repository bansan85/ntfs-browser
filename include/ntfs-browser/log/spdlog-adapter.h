#pragma once

// Opt-in adapters from spdlog to Log::ISink. This is the only public header
// that includes spdlog. The library itself never includes it, and nothing
// here is exported: every function is compiled in the consumer's build,
// against the consumer's spdlog. The library only ever sees an ISink, so
// the consumer's spdlog version, linkage and SPDLOG_WCHAR_FILENAMES need
// not match the library's.
//
// A spdlog sink next to the console target:
//   Log::SetSinks({Log::MakeConsoleSink(Log::Level::Warn),
//                  Log::MakeSpdlogSink(file, Log::Level::Debug)});
// A logger the consumer already owns:
//   Log::SetSinks({Log::MakeSpdlogLogger(spdlog::default_logger())});

#include <memory>
#include <string_view>
#include <utility>

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/sink.h>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Log::Adapter {

// spdlog counterpart of a library level.
inline spdlog::level::level_enum ToSpdlog(Level level) noexcept {
  switch (level) {
    case Level::Error:
      return spdlog::level::err;
    case Level::Warn:
      return spdlog::level::warn;
    case Level::Info:
      return spdlog::level::info;
    case Level::Debug:
      return spdlog::level::debug;
    case Level::Trace:
      return spdlog::level::trace;
    case Level::Off:
      break;
  }
  return spdlog::level::off;
}

// Forwards the library's messages to one spdlog sink.
class SinkAdapter final : public ISink {
 public:
  SinkAdapter(spdlog::sink_ptr sink, Level threshold)
      : sink_(std::move(sink)), threshold_(threshold) {}

  [[nodiscard]] bool ShouldLog(Level level) const override {
    // Off is a threshold, never a message level, and it silences the sink.
    return level != Level::Off && level <= threshold_ &&
           sink_->should_log(ToSpdlog(level));
  }

  void Write(Level level, std::string_view message) override {
    const spdlog::details::log_msg msg(
        spdlog::source_loc{},
        spdlog::string_view_t(logger_name.data(), logger_name.size()),
        ToSpdlog(level), spdlog::string_view_t(message.data(), message.size()));
    sink_->log(msg);
  }

  void Flush() override { sink_->flush(); }

 private:
  spdlog::sink_ptr sink_;
  Level threshold_;
};

// Emits the library's messages through a logger the consumer owns.
class LoggerAdapter final : public ISink {
 public:
  explicit LoggerAdapter(std::shared_ptr<spdlog::logger> logger)
      : logger_(std::move(logger)) {}

  [[nodiscard]] bool ShouldLog(Level level) const override {
    return level != Level::Off && logger_->should_log(ToSpdlog(level));
  }

  void Write(Level level, std::string_view message) override {
    logger_->log(ToSpdlog(level),
                 spdlog::string_view_t(message.data(), message.size()));
  }

  void Flush() override { logger_->flush(); }

 private:
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Log::Adapter

namespace NTFS_BROWSER_NAMESPACE::Log {

// Forwards the library's messages to a spdlog sink. A message must pass
// threshold first, then the sink's own level. Returns null if sink is null.
inline std::shared_ptr<ISink> MakeSpdlogSink(spdlog::sink_ptr sink,
                                             Level threshold = Level::Trace) {
  if (!sink) {
    return nullptr;
  }
  return std::make_shared<Adapter::SinkAdapter>(std::move(sink), threshold);
}

// Emits the library's messages through a logger the consumer already owns,
// with that logger's own level, sinks and formatting. The library shares
// ownership, so the logger outlives the consumer's reference to it.
// Returns null if logger is null.
inline std::shared_ptr<ISink>
    MakeSpdlogLogger(std::shared_ptr<spdlog::logger> logger) {
  if (!logger) {
    return nullptr;
  }
  return std::make_shared<Adapter::LoggerAdapter>(std::move(logger));
}

}  // namespace NTFS_BROWSER_NAMESPACE::Log
