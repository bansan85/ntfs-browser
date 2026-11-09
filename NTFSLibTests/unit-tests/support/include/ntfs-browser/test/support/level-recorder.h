#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "log/log.h"
#include "log/sink.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Support {

// Keeps each message the library logs with its level.
class LevelRecorder final : public NTFS_BROWSER_NAMESPACE::Log::ISink {
 public:
  [[nodiscard]] bool
      ShouldLog(NTFS_BROWSER_NAMESPACE::Log::Level level) const override {
    return level != NTFS_BROWSER_NAMESPACE::Log::Level::Off;
  }

  void Write(NTFS_BROWSER_NAMESPACE::Log::Level level,
             std::string_view message) override {
    messages_.emplace_back(level, std::string{message});
  }

  void Flush() override {}

  [[nodiscard]] bool Logged(NTFS_BROWSER_NAMESPACE::Log::Level level) const {
    return std::ranges::any_of(messages_, [level](const auto& message) {
      return message.first == level;
    });
  }

  // How many messages of level hold text.
  [[nodiscard]] size_t Count(NTFS_BROWSER_NAMESPACE::Log::Level level,
                             std::string_view text) const {
    return static_cast<size_t>(
        std::ranges::count_if(messages_, [level, text](const auto& message) {
          return message.first == level &&
                 message.second.find(text) != std::string::npos;
        }));
  }

 private:
  std::vector<std::pair<NTFS_BROWSER_NAMESPACE::Log::Level, std::string>>
      messages_;
};

// Makes a LevelRecorder the library's only sink while it lives, and puts the
// capture sink back after it.
class RecordLevels {
 public:
  RecordLevels() : recorder_(std::make_shared<LevelRecorder>()) {
    NTFS_BROWSER_NAMESPACE::Log::SetSinks({recorder_});
  }

  RecordLevels(const RecordLevels&) = delete;
  RecordLevels(RecordLevels&&) = delete;
  RecordLevels& operator=(const RecordLevels&) = delete;
  RecordLevels& operator=(RecordLevels&&) = delete;

  ~RecordLevels() { InstallCaptureSink(); }

  [[nodiscard]] const LevelRecorder& Levels() const { return *recorder_; }

 private:
  std::shared_ptr<LevelRecorder> recorder_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Support
