#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <frozen/bits/elsa_std.h>
#include <frozen/unordered_map.h>
#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"

// Config::file_path is handed to the file sink as-is. The build defines
// SPDLOG_WCHAR_FILENAMES on Windows for that: without it spdlog takes a
// narrow name and opens it through the active ANSI code page.
static_assert(
    std::is_same_v<spdlog::filename_t, std::filesystem::path::string_type>,
    "spdlog's filename type must match the platform's native path "
    "type; check SPDLOG_WCHAR_FILENAMES");

namespace NTFS_BROWSER_NAMESPACE::Log {
namespace {

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
spdlog::level::level_enum ToSpdlog(Log::Level level) noexcept {
  switch (level) {
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

// Writes to spdlog sinks. Warn and the more severe levels go to severe,
// the others to mild; the file target passes one sink for both. The sinks
// are never asked about their own level: threshold is the only filter, so
// the console's stderr half and stdout half cannot repeat a message.
class SpdlogTargetSink final : public ISink {
 public:
  SpdlogTargetSink(std::shared_ptr<spdlog::sinks::sink> severe,
                   std::shared_ptr<spdlog::sinks::sink> mild,
                   Log::Level threshold)
      : severe_(std::move(severe)),
        mild_(std::move(mild)),
        threshold_(threshold) {}

  [[nodiscard]] bool ShouldLog(Log::Level level) const override {
    // Off is a threshold, never a message level.
    return level != Log::Level::Off && level <= threshold_;
  }

  void Write(Log::Level level, std::string_view message) override {
    const spdlog::details::log_msg msg(
        spdlog::source_loc{},
        spdlog::string_view_t(Log::logger_name.data(), Log::logger_name.size()),
        ToSpdlog(level), spdlog::string_view_t(message.data(), message.size()));
    (level <= Log::Level::Warn ? severe_ : mild_)->log(msg);
  }

  void Flush() override {
    severe_->flush();
    if (mild_ != severe_) {
      mild_->flush();
    }
  }

 private:
  std::shared_ptr<spdlog::sinks::sink> severe_;
  std::shared_ptr<spdlog::sinks::sink> mild_;
  Log::Level threshold_;
};

// The sinks the library offers each message to. Deliberately never
// destroyed: objects with static storage duration log from their
// destructors, and sinks destroyed before them would be used after their
// lifetime ended.
struct SinkHolder {
  std::vector<std::shared_ptr<ISink>> sinks;
  // False until a call installs a set, so the first message can install
  // the default configuration.
  bool is_installed = false;
  // Set when every sink is a built-in target, whose threshold never
  // changes: the least severe level any of them accepts. IsEnabled() then
  // answers with one comparison instead of one virtual call per sink. A
  // set with a sink of the consumer's has none, since that sink may change
  // its mind between two messages.
  std::optional<Log::Level> fixed_threshold;
};

SinkHolder& Holder() {
  // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
  static auto* const holder = new SinkHolder();
  return *holder;
}

// Makes sinks the library's set. The previous sinks are released here, and
// a destructor of theirs that throws is the consumer's problem, not a
// reason to leave the new set uninstalled.
void Install(std::vector<std::shared_ptr<ISink>> sinks,
             std::optional<Log::Level> fixed_threshold) noexcept {
  SinkHolder& holder = Holder();
  holder.sinks.swap(sinks);
  holder.fixed_threshold = fixed_threshold;
  holder.is_installed = true;
  try {
    sinks.clear();
  } catch (...) {
    return;
  }
}

// Installs the targets config asks for, which replaces any set a previous
// call left behind. Returns false if the file sink could not be opened; the
// console target is installed anyway.
bool Apply(const Log::Config& config) noexcept {
  bool is_ok = true;

  try {
    std::vector<std::shared_ptr<ISink>> sinks;
    // Enumerators run from most to least severe, so the larger one is the
    // less severe.
    Log::Level least_severe = Log::Level::Off;

    if (config.console_level != Log::Level::Off) {
      sinks.push_back(Log::MakeConsoleSink(config.console_level));
      least_severe = (std::max)(least_severe, config.console_level);
    }

    if (config.file_level != Log::Level::Off) {
      std::shared_ptr<ISink> file =
          Log::MakeFileSink(config.file_path, config.file_level);
      if (file) {
        sinks.push_back(std::move(file));
        least_severe = (std::max)(least_severe, config.file_level);
      } else {
        is_ok = false;
      }
    }

    Install(std::move(sinks), least_severe);
  } catch (...) {
    return false;
  }

  return is_ok;
}

// The installed sinks, with the default configuration installed on first
// use.
const SinkHolder& EnsureSinks() {
  const SinkHolder& holder = Holder();
  if (!holder.is_installed) {
    Apply(Log::Config{});
  }
  return holder;
}

// True if text spells out the ASCII string ascii. Every keyword --log
// accepts is ASCII, so one parser can serve both a narrow and a wide argv.
template <typename CharT>
bool EqualsAscii(std::basic_string_view<CharT> text,
                 std::string_view ascii) noexcept {
  return std::equal(
      text.begin(), text.end(), ascii.begin(), ascii.end(),
      [](CharT lhs, char rhs) { return lhs == static_cast<CharT>(rhs); });
}

// Maps a --log level name onto its level. False if the name is unknown.
template <typename CharT>
bool ParseLevel(std::basic_string_view<CharT> text,
                Log::Level& level) noexcept {
  for (const auto& [name, value] : level_names) {
    if (EqualsAscii(text, name)) {
      level = value;
      return true;
    }
  }
  return false;
}

}  // namespace

void Exception(const std::exception& exception) noexcept {
  std::string_view message(exception.what());
  // Several throw sites end their message with a newline. spdlog adds its
  // own, so without this one exception would print a blank line after it.
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r')) {
    message.remove_suffix(1);
  }

  Detail::Emit(Level::Error, message);
}

bool Configure(const Config& config) noexcept { return Apply(config); }

void SetSinks(std::span<const std::shared_ptr<ISink>> sinks) noexcept {
  try {
    std::vector<std::shared_ptr<ISink>> installed;
    installed.reserve(sinks.size());
    for (const std::shared_ptr<ISink>& sink : sinks) {
      if (sink) {
        installed.push_back(sink);
      }
    }
    Install(std::move(installed), std::nullopt);
  } catch (...) {
    // Nothing is left to report the failure with: the previous set stays.
    return;
  }
}

std::shared_ptr<ISink> MakeConsoleSink(Level threshold) {
  // The _st sinks take no lock, which the library's single-threaded
  // contract allows and its per-message volume wants.
  const auto out = std::make_shared<spdlog::sinks::stdout_sink_st>();
  out->set_pattern(std::string(console_pattern));
  const auto err = std::make_shared<spdlog::sinks::stderr_sink_st>();
  err->set_pattern(std::string(console_pattern));
  return std::make_shared<SpdlogTargetSink>(err, out, threshold);
}

std::shared_ptr<ISink> MakeFileSink(const std::filesystem::path& path,
                                    Level threshold) {
  try {
    // Appends: a second Configure() with the same path must not wipe what
    // the first one already wrote.
    const auto file = std::make_shared<spdlog::sinks::basic_file_sink_st>(
        path.native(), false);
    file->set_pattern(std::string(file_pattern));
    return std::make_shared<SpdlogTargetSink>(file, file, threshold);
  } catch (...) {
    return nullptr;
  }
}

namespace {

// ParseOption(), over whichever character type the executable's argv has.
template <typename CharT>
bool ParseOptionImpl(std::basic_string_view<CharT> arg,
                     Config& config) noexcept {
  if (arg.size() < option_prefix.size() ||
      !EqualsAscii(arg.substr(0, option_prefix.size()), option_prefix)) {
    return false;
  }

  constexpr auto separator = static_cast<CharT>(':');

  const std::basic_string_view<CharT> value = arg.substr(option_prefix.size());
  const size_t target_end = value.find(separator);
  if (target_end == std::basic_string_view<CharT>::npos) {
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
  if (!ParseLevel(level_text, level)) {
    return false;
  }

  // A path field belongs to the file target only, and an empty one names
  // no file at all.
  if (has_path && (!EqualsAscii(target, file_target) || path.empty())) {
    return false;
  }

  if (EqualsAscii(target, console_target)) {
    config.console_level = level;
    return true;
  }

  if (!EqualsAscii(target, file_target)) {
    return false;
  }

  // Built before anything is committed: the conversion allocates, and
  // config must come back untouched whenever this returns false.
  std::filesystem::path file_path;
  if (has_path) {
    try {
      file_path.assign(path);
    } catch (...) {
      return false;
    }
  }

  config.file_level = level;
  if (has_path) {
    config.file_path = std::move(file_path);
  }
  return true;
}

}  // namespace

bool ParseOption(std::string_view arg, Config& config) noexcept {
  return ParseOptionImpl(arg, config);
}

#ifdef _WIN32
bool ParseOption(std::wstring_view arg, Config& config) noexcept {
  return ParseOptionImpl(arg, config);
}
#endif

}  // namespace NTFS_BROWSER_NAMESPACE::Log

namespace NTFS_BROWSER_NAMESPACE::Log::Detail {

namespace {

// Set by SilenceCurrentThread(), once per thread that must never log.
thread_local bool current_thread_silenced = false;

// Salvaged conditions this thread met, see RecoveredCount(). Per thread, so a
// worker thread of the library never races the caller's count.
thread_local std::uint64_t recovered_count = 0;

// Where this thread's lines go instead of the sinks, while a CaptureScope
// lives. Null otherwise.
thread_local CapturedLog* current_capture = nullptr;

}  // namespace

void NoteRecovered() noexcept { recovered_count++; }

std::uint64_t RecoveredCount() noexcept { return recovered_count; }

void ForgetRecoveredSince(const CapturedLog::Mark& mark) noexcept {
  recovered_count = mark.recovered;
}

void SilenceCurrentThread() noexcept { current_thread_silenced = true; }

bool IsEnabled(Level level) noexcept {
  if (current_thread_silenced) {
    return false;
  }

  // Off is a threshold, not a message level: no message is written at it.
  if (level == Level::Off) {
    return false;
  }

  // A capture never asks the sinks, which belong to another thread.
  if (current_capture != nullptr) {
    return (current_capture->Levels() & LevelBit(level)) != 0;
  }

  try {
    const SinkHolder& holder = EnsureSinks();
    if (holder.fixed_threshold) {
      return level <= *holder.fixed_threshold;
    }

    return std::ranges::any_of(holder.sinks,
                               [level](const std::shared_ptr<ISink>& sink) {
                                 try {
                                   return sink->ShouldLog(level);
                                 } catch (...) {
                                   // A sink that cannot answer is taken not to
                                   // want the message.
                                   return false;
                                 }
                               });
  } catch (...) {
    return false;
  }
}

void Emit(Level level, std::string_view message) noexcept {
  if (current_thread_silenced) {
    return;
  }

  if (current_capture != nullptr) {
    current_capture->Append(level, message);
    return;
  }

  try {
    // A copy: a sink may replace the set from inside Write(), which would
    // otherwise invalidate this loop and free the sink that is running.
    const std::vector<std::shared_ptr<ISink>> sinks = EnsureSinks().sinks;
    for (const std::shared_ptr<ISink>& sink : sinks) {
      try {
        if (sink->ShouldLog(level)) {
          sink->Write(level, message);
          // The sinks are never destroyed at exit, so nothing else would
          // flush a file sink: every line is flushed as it is written.
          sink->Flush();
        }
      } catch (...) {
        // One failing sink MUST NOT keep the next one from the line.
        continue;
      }
    }
  } catch (...) {
    // Nothing is left to report the failure with, so the line is dropped.
    return;
  }
}

LevelMask EnabledLevels() noexcept {
  LevelMask levels = 0;
  for (const Level level :
       {Level::Error, Level::Warn, Level::Info, Level::Debug, Level::Trace}) {
    if (IsEnabled(level)) {
      levels |= LevelBit(level);
    }
  }
  return levels;
}

void CapturedLog::Reset(LevelMask levels) noexcept {
  text_.clear();
  lines_.clear();
  levels_ = levels;
}

void CapturedLog::Append(Level level, std::string_view message) noexcept {
  try {
    text_.append(message);
    lines_.push_back({.level = level, .end = text_.size()});
  } catch (...) {
    // The line is lost, as a failed format loses its own.
    text_.resize(lines_.empty() ? 0 : lines_.back().end);
  }
}

CapturedLog::Mark CapturedLog::End() const noexcept {
  return {.lines = lines_.size(), .recovered = recovered_count};
}

void CapturedLog::Replay(const Mark& from, const Mark& to) const noexcept {
  const size_t last = (std::min)(to.lines, lines_.size());
  for (size_t index = from.lines; index < last; index++) {
    // index < last <= lines_.size().
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    const Line& line = lines_[index];
    const size_t begin = index == 0 ? 0 : lines_.at(index - 1).end;
    Emit(line.level, std::string_view(text_).substr(begin, line.end - begin));
  }
  recovered_count += to.recovered - from.recovered;
}

CaptureScope::CaptureScope(CapturedLog& log) noexcept
    : previous_(current_capture) {
  current_capture = &log;
}

CaptureScope::~CaptureScope() { current_capture = previous_; }

}  // namespace NTFS_BROWSER_NAMESPACE::Log::Detail
