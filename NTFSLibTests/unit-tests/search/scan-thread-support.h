#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/types.h>

#include "log/ntfs-common.h"
#include "log/sink.h"
#include "scan-support.h"

namespace NTFS_BROWSER_NAMESPACE::Test::Search {

// Options for a scan that runs a scripted engine: the image only has to exist.
inline NTFS_BROWSER_NAMESPACE::Options ScriptedOptions(const TempImage& image) {
  NTFS_BROWSER_NAMESPACE::Options options;
  options.target = image.Target();
  return options;
}

// What a test callback keeps of the batches.
struct Collected {
  std::vector<std::uint64_t> records;
  std::vector<std::size_t> sizes;
  std::size_t calls{0};
  std::size_t last_calls{0};
  NTFS_BROWSER_NAMESPACE::Progress progress;
};

// Makes scan keep every batch in collected.
inline void CollectInto(NTFS_BROWSER_NAMESPACE::Scan& scan,
                        Collected& collected) {
  scan.OnBatch([&collected](const NTFS_BROWSER_NAMESPACE::Batch& batch) {
    ++collected.calls;
    collected.sizes.push_back(batch.entries.size());
    if (batch.last) {
      ++collected.last_calls;
    }
    collected.progress = batch.progress;
    for (const NTFS_BROWSER_NAMESPACE::Entry& entry : batch.entries) {
      collected.records.push_back(entry.record);
    }
  });
}

// The numbers 0 to count - 1.
inline std::vector<std::uint64_t> Sequence(std::uint64_t count) {
  std::vector<std::uint64_t> numbers;
  for (std::uint64_t number = 0; number < count; ++number) {
    numbers.push_back(number);
  }
  return numbers;
}

// One line a sink received, with the thread that wrote it.
struct SeenLine {
  std::thread::id thread;
  NTFS_BROWSER_NAMESPACE::Log::Level level;
  std::string text;
};

// Keeps every line with the id of the thread that called Write().
class ThreadSink final : public NTFS_BROWSER_NAMESPACE::Log::ISink {
 public:
  [[nodiscard]] bool
      ShouldLog(NTFS_BROWSER_NAMESPACE::Log::Level level) const override {
    return level != NTFS_BROWSER_NAMESPACE::Log::Level::Off;
  }

  void Write(NTFS_BROWSER_NAMESPACE::Log::Level level,
             std::string_view message) override {
    const std::scoped_lock lock(mutex_);
    lines_.push_back({.thread = std::this_thread::get_id(),
                      .level = level,
                      .text = std::string(message)});
  }

  void Flush() override {}

  [[nodiscard]] std::vector<SeenLine> Lines() {
    const std::scoped_lock lock(mutex_);
    return lines_;
  }

  [[nodiscard]] bool Has(std::string_view text) {
    const std::scoped_lock lock(mutex_);
    return std::ranges::any_of(
        lines_, [text](const SeenLine& line) { return line.text == text; });
  }

 private:
  std::mutex mutex_;
  std::vector<SeenLine> lines_;
};

// Makes sink the only sink of the library, and puts the capturing sink of the
// tests back afterwards.
class SinkScope final {
 public:
  explicit SinkScope(
      const std::shared_ptr<NTFS_BROWSER_NAMESPACE::Log::ISink>& sink) {
    NTFS_BROWSER_NAMESPACE::Log::SetSinks({sink});
  }

  SinkScope(const SinkScope&) = delete;
  SinkScope& operator=(const SinkScope&) = delete;
  SinkScope(SinkScope&&) = delete;
  SinkScope& operator=(SinkScope&&) = delete;

  ~SinkScope() { NTFS_BROWSER_NAMESPACE::Test::Support::InstallCaptureSink(); }
};

// Waits until test() is true, at most a long time.
template <class Test>
bool WaitUntil(Test test) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!test()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// Cancels scan after a long time, so that a Run() that fails to wake ends the
// test as Cancelled instead of hanging it.
class Watchdog final {
 public:
  explicit Watchdog(NTFS_BROWSER_NAMESPACE::Scan& scan)
      : thread_([&scan](const std::stop_token& stop) {
          std::mutex mutex;
          std::condition_variable_any quiet;
          std::unique_lock lock(mutex);
          if (!quiet.wait_for(lock, stop, std::chrono::seconds(60),
                              [] { return false; }) &&
              !stop.stop_requested()) {
            scan.Cancel();
          }
        }) {}

  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;
  Watchdog(Watchdog&&) = delete;
  Watchdog& operator=(Watchdog&&) = delete;
  ~Watchdog() = default;

 private:
  std::jthread thread_;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Test::Search
