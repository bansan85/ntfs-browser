#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "log/ntfs-common.h"
#include "log/worker-log.h"

namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Detail = NTFS_BROWSER_NAMESPACE::Log::Detail;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;

namespace {

// One line a sink received, with the thread that wrote it.
struct SeenLine {
  std::thread::id thread;
  std::string text;
};

// Keeps every line with the id of the thread that called Write().
class ThreadSink final : public Log::ISink {
 public:
  [[nodiscard]] bool ShouldLog(Log::Level level) const override {
    return level != Log::Level::Off;
  }

  void Write(Log::Level, std::string_view message) override {
    const std::scoped_lock lock(mutex_);
    lines_.push_back(
        {.thread = std::this_thread::get_id(), .text = std::string(message)});
  }

  void Flush() override {}

  [[nodiscard]] std::vector<SeenLine> Lines() {
    const std::scoped_lock lock(mutex_);
    return lines_;
  }

 private:
  std::mutex mutex_;
  std::vector<SeenLine> lines_;
};

// Makes sink the only sink of the library, then puts the capturing sink of the
// tests back.
class SinkScope final {
 public:
  explicit SinkScope(const std::shared_ptr<Log::ISink>& sink) {
    Log::SetSinks({sink});
  }

  SinkScope(const SinkScope&) = delete;
  SinkScope& operator=(const SinkScope&) = delete;
  SinkScope(SinkScope&&) = delete;
  SinkScope& operator=(SinkScope&&) = delete;

  ~SinkScope() { TestSupport::InstallCaptureSink(); }
};

// A step that the worker waits for and the test takes.
class Step final {
 public:
  void Release() {
    {
      const std::scoped_lock lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

  void Wait() {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return released_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool released_{false};
};

constexpr Detail::LevelMask all_levels =
    Detail::LevelBit(Log::Level::Error) | Detail::LevelBit(Log::Level::Warn) |
    Detail::LevelBit(Log::Level::Info) | Detail::LevelBit(Log::Level::Debug) |
    Detail::LevelBit(Log::Level::Trace);

}  // namespace

TEST_CASE("a worker log replays the lines of the worker on the owner thread",
          "[log][worker]") {
  constexpr std::size_t count = 5000;
  const auto sink = std::make_shared<ThreadSink>();
  const SinkScope scope(sink);
  Detail::WorkerLog log;
  log.Prepare(all_levels);

  std::thread worker([&log] {
    log.Begin();
    for (std::size_t number = 0; number < count; ++number) {
      Log::Info("line {}", number);
      log.Checkpoint();
    }
    log.End();
  });
  // Replay while the worker runs: a request is made, the worker answers.
  for (std::size_t round = 0; round < 200; ++round) {
    log.Replay();
    std::this_thread::yield();
  }
  worker.join();
  log.Replay();

  const std::vector<SeenLine> lines = sink->Lines();
  REQUIRE(lines.size() == count);
  for (std::size_t number = 0; number < count; ++number) {
    CHECK(lines.at(number).text == "line " + std::to_string(number));
  }
  CHECK(std::ranges::all_of(lines, [](const SeenLine& line) {
    return line.thread == std::this_thread::get_id();
  }));
}

TEST_CASE("a worker log hands lines over at a checkpoint after a request",
          "[log][worker]") {
  const auto sink = std::make_shared<ThreadSink>();
  const SinkScope scope(sink);
  Detail::WorkerLog log;
  log.Prepare(all_levels);
  Step logged;
  Step requested;
  Step handed;
  Step proceed;

  std::thread worker([&] {
    log.Begin();
    Log::Info("first");
    logged.Release();
    requested.Wait();
    log.Checkpoint();
    handed.Release();
    proceed.Wait();
    Log::Info("second");
    log.End();
  });
  logged.Wait();

  // Nothing is handed over yet: the worker was not asked.
  log.Replay();
  CHECK(sink->Lines().empty());

  // The first Replay() asked. The checkpoint hands the line over.
  requested.Release();
  handed.Wait();
  log.Replay();
  REQUIRE(sink->Lines().size() == 1);
  CHECK(sink->Lines().front().text == "first");

  proceed.Release();
  worker.join();
  log.Replay();
  REQUIRE(sink->Lines().size() == 2);
  CHECK(sink->Lines().back().text == "second");
}

TEST_CASE("a worker log keeps only the levels it was prepared with",
          "[log][worker]") {
  const auto sink = std::make_shared<ThreadSink>();
  const SinkScope scope(sink);
  Detail::WorkerLog log;
  log.Prepare(Detail::LevelBit(Log::Level::Error));

  std::thread worker([&log] {
    log.Begin();
    Log::Info("dropped");
    Log::Error("kept");
    log.End();
  });
  worker.join();
  log.Replay();

  REQUIRE(sink->Lines().size() == 1);
  CHECK(sink->Lines().front().text == "kept");
}

TEST_CASE("a worker log hands the salvaged count to the owner",
          "[log][worker]") {
  const auto sink = std::make_shared<ThreadSink>();
  const SinkScope scope(sink);
  Detail::WorkerLog log;
  log.Prepare(all_levels);

  std::thread worker([&log] {
    log.Begin();
    Log::Recoverable(true, "salvaged {}", 1);
    Log::Recoverable(true, "salvaged {}", 2);
    log.End();
  });
  worker.join();
  const std::uint64_t before = Detail::RecoveredCount();
  log.Replay();

  CHECK(Detail::RecoveredCount() - before == 2);
  CHECK(sink->Lines().size() == 2);
}

TEST_CASE("a worker log with no lines replays nothing", "[log][worker]") {
  const auto sink = std::make_shared<ThreadSink>();
  const SinkScope scope(sink);
  Detail::WorkerLog log;
  log.Prepare(all_levels);

  std::thread worker([&log] {
    log.Begin();
    log.Checkpoint();
    log.End();
  });
  worker.join();
  log.Replay();
  log.Replay();

  CHECK(sink->Lines().empty());
}
