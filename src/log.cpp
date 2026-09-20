#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/log.h>

#include "ntfs-common.h"

namespace NtfsBrowser
{
namespace
{

// The console target reproduces the message and nothing else, so what a
// consumer sees is exactly what the library wrote.
constexpr std::string_view kConsolePattern = "%v";
// A log file is read long after the run, so each line is dated and says
// which level produced it.
constexpr std::string_view kFilePattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";

// Level names --log accepts, paired with the level each one selects.
constexpr std::array<std::pair<std::string_view, Log::Level>, 6> kLevelNames{
    {{"off", Log::Level::kOff},
     {"error", Log::Level::kError},
     {"warn", Log::Level::kWarn},
     {"info", Log::Level::kInfo},
     {"debug", Log::Level::kDebug},
     {"trace", Log::Level::kTrace}}};

// Target names --log accepts.
constexpr std::string_view kConsoleTarget = "console";
constexpr std::string_view kFileTarget = "file";

// spdlog counterpart of a public level.
spdlog::level::level_enum ToSpdlog(Log::Level level) noexcept
{
  switch (level)
  {
    case Log::Level::kError:
      return spdlog::level::err;
    case Log::Level::kWarn:
      return spdlog::level::warn;
    case Log::Level::kInfo:
      return spdlog::level::info;
    case Log::Level::kDebug:
      return spdlog::level::debug;
    case Log::Level::kTrace:
      return spdlog::level::trace;
    case Log::Level::kOff:
      break;
  }
  return spdlog::level::off;
}

// Wraps a sink and drops everything at or above an exclusive ceiling.
// spdlog gives a sink a minimum level only. Without this the stdout half
// of the console target would repeat every warning and error that its
// stderr half already printed.
class CeilingSink final : public spdlog::sinks::sink
{
 public:
  CeilingSink(std::shared_ptr<spdlog::sinks::sink> inner,
              spdlog::level::level_enum ceiling)
      : inner_(std::move(inner)), ceiling_(ceiling)
  {
  }

  CeilingSink(CeilingSink&&) = delete;
  CeilingSink(const CeilingSink&) = delete;
  CeilingSink& operator=(CeilingSink&&) = delete;
  CeilingSink& operator=(const CeilingSink&) = delete;
  ~CeilingSink() override = default;

  void log(const spdlog::details::log_msg& msg) override
  {
    if (msg.level >= ceiling_)
    {
      return;
    }
    inner_->log(msg);
  }

  void flush() override { inner_->flush(); }

  void set_pattern(const std::string& pattern) override
  {
    inner_->set_pattern(pattern);
  }

  void set_formatter(std::unique_ptr<spdlog::formatter> sink_formatter) override
  {
    inner_->set_formatter(std::move(sink_formatter));
  }

 private:
  std::shared_ptr<spdlog::sinks::sink> inner_;
  spdlog::level::level_enum ceiling_;
};

// Holds the library logger. Deliberately never destroyed: objects with
// static storage duration log from their destructors, and a logger
// destroyed before them would be used after its lifetime ended.
struct LoggerHolder
{
  std::shared_ptr<spdlog::logger> logger;
};

LoggerHolder& Holder()
{
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  static LoggerHolder* const holder = new LoggerHolder();
  return *holder;
}

// The least severe level any sink accepts, which is the level the logger
// itself must sit at for every sink to see what it asked for.
spdlog::level::level_enum
    LeastSevere(const std::vector<spdlog::sink_ptr>& sinks) noexcept
{
  spdlog::level::level_enum result = spdlog::level::off;
  for (const spdlog::sink_ptr& sink : sinks)
  {
    result = (std::min)(result, sink->level());
  }
  return result;
}

// Builds the console target: a stdout sink capped below warn, plus a
// stderr sink floored at warn, so each message lands on one stream only.
// The _st sinks take no lock, which the library's single-threaded
// contract allows and its per-cluster message volume wants.
void AddConsoleSinks(Log::Level level, std::vector<spdlog::sink_ptr>& sinks)
{
  auto out = std::make_shared<CeilingSink>(
      std::make_shared<spdlog::sinks::stdout_sink_st>(), spdlog::level::warn);
  out->set_level(ToSpdlog(level));
  out->set_pattern(std::string(kConsolePattern));
  sinks.push_back(out);

  auto err = std::make_shared<spdlog::sinks::stderr_sink_st>();
  err->set_level((std::max)(spdlog::level::warn, ToSpdlog(level)));
  err->set_pattern(std::string(kConsolePattern));
  sinks.push_back(err);
}

// Installs the sinks config asks for on a freshly built logger, which
// replaces any logger a previous call left behind. Returns false if the
// file sink could not be opened; the console target is installed anyway.
bool Apply(const Log::Config& config) noexcept
{
  bool ok = true;

  try
  {
    std::vector<spdlog::sink_ptr> sinks;

    if (config.console_level != Log::Level::kOff)
    {
      AddConsoleSinks(config.console_level, sinks);
    }

    if (config.file_level != Log::Level::kOff)
    {
      try
      {
        // Appends: a second Configure() with the same path must not wipe
        // what the first one already wrote.
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_st>(
            config.file_path, false);
        file->set_level(ToSpdlog(config.file_level));
        file->set_pattern(std::string(kFilePattern));
        sinks.push_back(file);
      }
      catch (...)
      {
        ok = false;
      }
    }

    auto logger = std::make_shared<spdlog::logger>(
        std::string(Log::kLoggerName), sinks.begin(), sinks.end());
    logger->set_level(LeastSevere(sinks));
    // The holder is never destroyed, so nothing would flush the file sink
    // at exit; every line is therefore flushed as it is written.
    logger->flush_on(spdlog::level::trace);

    spdlog::drop(std::string(Log::kLoggerName));
    spdlog::register_logger(logger);
    Holder().logger = std::move(logger);
  }
  catch (...)
  {
    return false;
  }

  return ok;
}

// The library logger, created with the default configuration on first use.
// Null only if even that could not be built.
spdlog::logger* EnsureLogger()
{
  LoggerHolder& holder = Holder();
  if (!holder.logger)
  {
    Apply(Log::Config{});
  }
  return holder.logger.get();
}

// Maps a --log level name onto its level. False if the name is unknown.
bool ParseLevel(std::string_view text, Log::Level& level) noexcept
{
  for (const auto& [name, value] : kLevelNames)
  {
    if (name == text)
    {
      level = value;
      return true;
    }
  }
  return false;
}

}  // namespace

void LogException(const std::exception& e) noexcept
{
  std::string_view message(e.what());
  // Several throw sites end their message with a newline. spdlog adds its
  // own, so without this one exception would print a blank line after it.
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
  {
    message.remove_suffix(1);
  }

  Log::Detail::Emit(Log::Level::kError, message);
}

}  // namespace NtfsBrowser

namespace NtfsBrowser::Log
{

bool Configure(const Config& config) noexcept { return Apply(config); }

bool ParseOption(std::string_view arg, Config& config) noexcept
{
  if (!arg.starts_with(kOptionPrefix))
  {
    return false;
  }

  const std::string_view value = arg.substr(kOptionPrefix.size());
  const size_t targetEnd = value.find(':');
  if (targetEnd == std::string_view::npos)
  {
    return false;
  }

  const std::string_view target = value.substr(0, targetEnd);
  const std::string_view rest = value.substr(targetEnd + 1);
  // Only the first two colons split the option, so "C:\dir\ntfs.log"
  // survives as one path field.
  const size_t levelEnd = rest.find(':');
  const std::string_view levelText = rest.substr(0, levelEnd);
  const bool hasPath = levelEnd != std::string_view::npos;
  const std::string_view path =
      hasPath ? rest.substr(levelEnd + 1) : std::string_view{};

  Level level = Level::kOff;
  if (!ParseLevel(levelText, level))
  {
    return false;
  }

  // A path field belongs to the file target only, and an empty one names
  // no file at all.
  if (hasPath && (target != kFileTarget || path.empty()))
  {
    return false;
  }

  if (target == kConsoleTarget)
  {
    config.console_level = level;
    return true;
  }

  if (target != kFileTarget)
  {
    return false;
  }

  // Built before anything is committed: assign() allocates, and config
  // must come back untouched whenever this returns false.
  std::string filePath;
  if (hasPath)
  {
    try
    {
      filePath.assign(path);
    }
    catch (...)
    {
      return false;
    }
  }

  config.file_level = level;
  if (hasPath)
  {
    config.file_path = std::move(filePath);
  }
  return true;
}

}  // namespace NtfsBrowser::Log

namespace NtfsBrowser::Log::Detail
{

bool IsEnabled(Level level) noexcept
{
  try
  {
    const spdlog::logger* const logger = EnsureLogger();
    return logger != nullptr && logger->should_log(ToSpdlog(level));
  }
  catch (...)
  {
    return false;
  }
}

void Emit(Level level, std::string_view message) noexcept
{
  try
  {
    spdlog::logger* const logger = EnsureLogger();
    if (logger == nullptr)
    {
      return;
    }
    logger->log(ToSpdlog(level),
                spdlog::string_view_t(message.data(), message.size()));
  }
  catch (...)
  {
    // Nothing is left to report the failure with, so the line is dropped.
    return;
  }
}

}  // namespace NtfsBrowser::Log::Detail
