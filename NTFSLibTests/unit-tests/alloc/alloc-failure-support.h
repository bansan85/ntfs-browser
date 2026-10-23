#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/namespace.h>

#include "test-log-sink.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Attr {

struct HeaderCommon;

}  // namespace Attr
}  // namespace NTFS_BROWSER_NAMESPACE

namespace NTFS_BROWSER_NAMESPACE::Tests::AllocSupport {

// Records each message of the library with its level.
class LevelSink final : public Log::ISink {
 public:
  struct Line {
    Log::Level level;
    std::string text;
  };

  [[nodiscard]] bool ShouldLog(Log::Level level) const override {
    return level != Log::Level::Off;
  }

  void Write(Log::Level level, std::string_view message) override {
    lines.push_back({level, std::string(message)});
  }

  void Flush() override {}

  // Lines that hold text.
  [[nodiscard]] size_t Count(std::string_view text) const {
    size_t count = 0;
    for (const Line& line : lines) {
      count += line.text.find(text) != std::string::npos ? 1 : 0;
    }
    return count;
  }

  // Level of the first line that holds text.
  [[nodiscard]] std::optional<Log::Level> LevelOf(std::string_view text) const {
    for (const Line& line : lines) {
      if (line.text.find(text) != std::string::npos) {
        return line.level;
      }
    }
    return std::nullopt;
  }

  std::vector<Line> lines;
};

// Makes sink hear the library next to the capture sink, for as long as it
// lives, then puts the capture sink back alone.
class ScopedSink {
 public:
  explicit ScopedSink(std::shared_ptr<LevelSink> sink)
      : sink_(std::move(sink)) {
    Log::SetSinks({Tests::CaptureSinkHandle(), sink_});
  }

  ScopedSink(const ScopedSink&) = delete;
  ScopedSink& operator=(const ScopedSink&) = delete;

  ~ScopedSink() { Tests::InstallCaptureSink(); }

 private:
  std::shared_ptr<LevelSink> sink_;
};

// A LevelSink that hears the library for as long as it lives.
class CapturedLevels {
 public:
  CapturedLevels() : sink_(std::make_shared<LevelSink>()), scoped_(sink_) {}

  // Lines that hold text.
  [[nodiscard]] size_t Count(std::string_view text) const {
    return sink_->Count(text);
  }

  // Level of the first line that holds text.
  [[nodiscard]] std::optional<Log::Level> LevelOf(std::string_view text) const {
    return sink_->LevelOf(text);
  }

 private:
  std::shared_ptr<LevelSink> sink_;
  ScopedSink scoped_;
};

// A raw attribute callback that throws, as a user callback can.
inline void ThrowingCallback(const Attr::HeaderCommon& /*header*/,
                             bool& /*discard*/) {
  throw std::runtime_error("callback failed");
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::AllocSupport
