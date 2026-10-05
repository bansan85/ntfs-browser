#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <frozen/bits/elsa_std.h>
#include <frozen/unordered_map.h>
#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/formatter.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/log.h>

#include "ntfs-common.h"

// Config::file_path is handed to the file sink as-is. The build defines
// SPDLOG_WCHAR_FILENAMES on Windows for that: without it spdlog takes a
// narrow name and opens it through the active ANSI code page.
static_assert(
    std::is_same_v<spdlog::filename_t, std::filesystem::path::string_type>,
    "spdlog's filename type must match the platform's native path "
    "type; check SPDLOG_WCHAR_FILENAMES");

namespace NtfsBrowser
{
namespace
{

// The console target reproduces the message and nothing else, so what a
// consumer sees is exactly what the library wrote.
constexpr std::string_view console_pattern = "%v";
// A log file is read long after the run, so each line is dated and says
// which level produced it.
constexpr std::string_view file_pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";

// Level names --log accepts, paired with the level each one selects.
constexpr frozen::unordered_map<std::string_view, Log::Level, 6> level_names{
    {"off", Log::Level::Off},     {"error", Log::Level::Error},
    {"warn", Log::Level::Warn},   {"info", Log::Level::Info},
    {"debug", Log::Level::Debug}, {"trace", Log::Level::Trace}};

// Target names --log accepts.
constexpr std::string_view console_target = "console";
constexpr std::string_view file_target = "file";

// spdlog counterpart of a public level.
spdlog::level::level_enum ToSpdlog(Log::Level level) noexcept
{
  switch (level)
  {
    case Log::Level::Error:
      return spdlog::level::err;
    case Log::Level::Warn:
      return spdlog::level::warn;
    case Log::Level::Info:
      return spdlog::level::info;
    case Log::Level::Debug:
      return spdlog::level::debug;
    case Log::Level::Trace:
      return spdlog::level::trace;
    case Log::Level::Off:
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
  static auto* const holder = new LoggerHolder();
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
  auto const out = std::make_shared<CeilingSink>(
      std::make_shared<spdlog::sinks::stdout_sink_st>(), spdlog::level::warn);
  out->set_level(ToSpdlog(level));
  out->set_pattern(std::string(console_pattern));
  sinks.push_back(out);

  auto const err = std::make_shared<spdlog::sinks::stderr_sink_st>();
  err->set_level((std::max)(spdlog::level::warn, ToSpdlog(level)));
  err->set_pattern(std::string(console_pattern));
  sinks.push_back(err);
}

// Installs the sinks config asks for on a freshly built logger, which
// replaces any logger a previous call left behind. Returns false if the
// file sink could not be opened; the console target is installed anyway.
bool Apply(const Log::Config& config) noexcept
{
  bool is_ok = true;

  try
  {
    std::vector<spdlog::sink_ptr> sinks;

    if (config.console_level != Log::Level::Off)
    {
      AddConsoleSinks(config.console_level, sinks);
    }

    if (config.file_level != Log::Level::Off)
    {
      try
      {
        // Appends: a second Configure() with the same path must not wipe
        // what the first one already wrote.
        auto const file = std::make_shared<spdlog::sinks::basic_file_sink_st>(
            config.file_path.native(), false);
        file->set_level(ToSpdlog(config.file_level));
        file->set_pattern(std::string(file_pattern));
        sinks.push_back(file);
      }
      catch (...)
      {
        is_ok = false;
      }
    }

    auto logger = std::make_shared<spdlog::logger>(
        std::string(Log::logger_name), sinks.begin(), sinks.end());
    logger->set_level(LeastSevere(sinks));
    // The holder is never destroyed, so nothing would flush the file sink
    // at exit; every line is therefore flushed as it is written.
    logger->flush_on(spdlog::level::trace);

    spdlog::drop(std::string(Log::logger_name));
    spdlog::register_logger(logger);
    Holder().logger = std::move(logger);
  }
  catch (...)
  {
    return false;
  }

  return is_ok;
}

// The library logger, created with the default configuration on first use.
// Null only if even that could not be built.
spdlog::logger* EnsureLogger()
{
  LoggerHolder const& holder = Holder();
  if (!holder.logger)
  {
    Apply(Log::Config{});
  }
  return holder.logger.get();
}

// True if text spells out the ASCII string ascii. Every keyword --log
// accepts is ASCII, so one parser can serve both a narrow and a wide argv.
template <typename CharT>
bool EqualsAscii(std::basic_string_view<CharT> text,
                 std::string_view ascii) noexcept
{
  return std::equal(text.begin(), text.end(), ascii.begin(), ascii.end(),
                    [](CharT lhs, char rhs)
                    { return lhs == static_cast<CharT>(rhs); });
}

// Maps a --log level name onto its level. False if the name is unknown.
template <typename CharT>
bool ParseLevel(std::basic_string_view<CharT> text, Log::Level& level) noexcept
{
  for (const auto& [name, value] : level_names)
  {
    if (EqualsAscii(text, name))
    {
      level = value;
      return true;
    }
  }
  return false;
}

}  // namespace

void LogException(const std::exception& exception) noexcept
{
  std::string_view message(exception.what());
  // Several throw sites end their message with a newline. spdlog adds its
  // own, so without this one exception would print a blank line after it.
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
  {
    message.remove_suffix(1);
  }

  Log::Detail::Emit(Log::Level::Error, message);
}

}  // namespace NtfsBrowser

namespace NtfsBrowser::Log
{

bool Configure(const Config& config) noexcept { return Apply(config); }

namespace
{

// ParseOption(), over whichever character type the executable's argv has.
template <typename CharT>
bool ParseOptionImpl(std::basic_string_view<CharT> arg, Config& config) noexcept
{
  if (arg.size() < option_prefix.size() ||
      !EqualsAscii(arg.substr(0, option_prefix.size()), option_prefix))
  {
    return false;
  }

  constexpr auto separator = static_cast<CharT>(':');

  const std::basic_string_view<CharT> value = arg.substr(option_prefix.size());
  const size_t target_end = value.find(separator);
  if (target_end == std::basic_string_view<CharT>::npos)
  {
    return false;
  }

  const std::basic_string_view<CharT> target = value.substr(0, target_end);
  const std::basic_string_view<CharT> rest = value.substr(target_end + 1);
  // Only the first two colons split the option, so "C:\dir\ntfs.log"
  // survives as one path field.
  const size_t level_end = rest.find(separator);
  const std::basic_string_view<CharT> level_text = rest.substr(0, level_end);
  const bool has_path = level_end != std::basic_string_view<CharT>::npos;
  const std::basic_string_view<CharT> path =
      has_path ? rest.substr(level_end + 1) : std::basic_string_view<CharT>{};

  Level level = Level::Off;
  if (!ParseLevel(level_text, level))
  {
    return false;
  }

  // A path field belongs to the file target only, and an empty one names
  // no file at all.
  if (has_path && (!EqualsAscii(target, file_target) || path.empty()))
  {
    return false;
  }

  if (EqualsAscii(target, console_target))
  {
    config.console_level = level;
    return true;
  }

  if (!EqualsAscii(target, file_target))
  {
    return false;
  }

  // Built before anything is committed: the conversion allocates, and
  // config must come back untouched whenever this returns false.
  std::filesystem::path file_path;
  if (has_path)
  {
    try
    {
      file_path.assign(path);
    }
    catch (...)
    {
      return false;
    }
  }

  config.file_level = level;
  if (has_path)
  {
    config.file_path = std::move(file_path);
  }
  return true;
}

}  // namespace

bool ParseOption(std::string_view arg, Config& config) noexcept
{
  return ParseOptionImpl(arg, config);
}

#ifdef _WIN32
bool ParseOption(std::wstring_view arg, Config& config) noexcept
{
  return ParseOptionImpl(arg, config);
}
#endif

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
