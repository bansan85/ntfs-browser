#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"
#include "test-log-sink.h"

namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Detail = NTFS_BROWSER_NAMESPACE::Log::Detail;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// Keeps every line it is given, with its level, and accepts the levels up to
// its threshold.
class LevelSink final : public Log::ISink {
 public:
  using Line = std::pair<Log::Level, std::string>;

  explicit LevelSink(Log::Level threshold) : threshold_(threshold) {}

  [[nodiscard]] bool ShouldLog(Log::Level level) const override {
    return level != Log::Level::Off && level <= threshold_;
  }

  void Write(Log::Level level, std::string_view message) override {
    lines_.emplace_back(level, std::string(message));
  }

  void Flush() override {}

  [[nodiscard]] const std::vector<Line>& Lines() const { return lines_; }

 private:
  Log::Level threshold_;
  std::vector<Line> lines_;
};

// Makes sink the only sink for the life of the object, then puts the capturing
// sink of the tests back.
class SinkScope {
 public:
  explicit SinkScope(std::shared_ptr<Log::ISink> sink) {
    Log::SetSinks({std::move(sink)});
  }

  SinkScope(const SinkScope&) = delete;
  SinkScope(SinkScope&&) = delete;
  SinkScope& operator=(const SinkScope&) = delete;
  SinkScope& operator=(SinkScope&&) = delete;

  ~SinkScope() { Tests::InstallCaptureSink(); }
};

constexpr Detail::LevelMask all_levels =
    Detail::LevelBit(Log::Level::Error) | Detail::LevelBit(Log::Level::Warn) |
    Detail::LevelBit(Log::Level::Info) | Detail::LevelBit(Log::Level::Debug) |
    Detail::LevelBit(Log::Level::Trace);

}  // namespace

TEST_CASE(
    "a capturing thread keeps its lines out of the sinks until they are "
    "replayed",
    "[log][capture]") {
  static_cast<void>(Tests::TakeCapturedLog());
  Detail::CapturedLog log;
  log.Reset(all_levels);
  const Detail::CapturedLog::Mark begin = log.End();
  {
    const Detail::CaptureScope scope(log);
    Log::Info("one {}", 1);
    Log::Warn("two");
  }
  CHECK(Tests::TakeCapturedLog().empty());

  log.Replay(begin, log.End());

  CHECK(Tests::TakeCapturedLog() == "one 1\ntwo\n");
}

TEST_CASE("a replay sends the lines between two marks, in order",
          "[log][capture]") {
  static_cast<void>(Tests::TakeCapturedLog());
  Detail::CapturedLog log;
  log.Reset(all_levels);
  Detail::CapturedLog::Mark first;
  Detail::CapturedLog::Mark second;
  {
    const Detail::CaptureScope scope(log);
    Log::Info("a");
    first = log.End();
    Log::Info("b");
    Log::Info("c");
    second = log.End();
    Log::Info("d");
  }

  log.Replay(first, second);

  CHECK(Tests::TakeCapturedLog() == "b\nc\n");
}

TEST_CASE("a replay from another thread than the capture is ordered",
          "[log][capture]") {
  static_cast<void>(Tests::TakeCapturedLog());
  Detail::CapturedLog log;
  log.Reset(all_levels);
  Detail::CapturedLog::Mark first;
  Detail::CapturedLog::Mark second;
  std::thread worker([&] {
    const Detail::CaptureScope scope(log);
    Log::Info("a");
    first = log.End();
    Log::Info("b");
    Log::Warn("c");
    second = log.End();
    Log::Info("d");
  });
  worker.join();

  CHECK(Tests::TakeCapturedLog().empty());
  log.Replay(first, second);

  CHECK(Tests::TakeCapturedLog() == "b\nc\n");
}

TEST_CASE("a capture keeps the levels it was given and answers for them",
          "[log][capture]") {
  Detail::CapturedLog log;
  log.Reset(Detail::LevelBit(Log::Level::Error) |
            Detail::LevelBit(Log::Level::Debug));
  {
    const Detail::CaptureScope scope(log);
    CHECK(Detail::IsEnabled(Log::Level::Error));
    CHECK_FALSE(Detail::IsEnabled(Log::Level::Warn));
    CHECK_FALSE(Detail::IsEnabled(Log::Level::Info));
    CHECK(Detail::IsEnabled(Log::Level::Debug));
    CHECK_FALSE(Detail::IsEnabled(Log::Level::Trace));
    CHECK_FALSE(Detail::IsEnabled(Log::Level::Off));
    CHECK(Log::IsLogged(Log::Level::Debug));
  }
  // Outside, the sinks answer again: the capturing sink wants everything.
  CHECK(Detail::IsEnabled(Log::Level::Info));
}

TEST_CASE("a capture drops the lines of the levels it does not keep",
          "[log][capture]") {
  static_cast<void>(Tests::TakeCapturedLog());
  Detail::CapturedLog log;
  log.Reset(Detail::LevelBit(Log::Level::Warn));
  const Detail::CapturedLog::Mark begin = log.End();
  {
    const Detail::CaptureScope scope(log);
    Log::Error("error");
    Log::Warn("warn");
    Log::Info("info");
    Log::Debug("debug");
    Log::Trace("trace");
  }

  log.Replay(begin, log.End());

  CHECK(Tests::TakeCapturedLog() == "warn\n");
}

TEST_CASE("the levels a capture is asked for are the ones the sinks accept",
          "[log][capture]") {
  CHECK(Detail::EnabledLevels() == all_levels);
  {
    const SinkScope sinks(std::make_shared<LevelSink>(Log::Level::Warn));
    CHECK(Detail::EnabledLevels() == (Detail::LevelBit(Log::Level::Error) |
                                      Detail::LevelBit(Log::Level::Warn)));
  }
  {
    const SinkScope sinks(std::make_shared<LevelSink>(Log::Level::Off));
    CHECK(Detail::EnabledLevels() == 0);
  }
  CHECK(Detail::EnabledLevels() == all_levels);
}

TEST_CASE("a replay offers each line to the sinks, which choose by level",
          "[log][capture]") {
  Detail::CapturedLog log;
  log.Reset(all_levels);
  const Detail::CapturedLog::Mark begin = log.End();
  {
    const Detail::CaptureScope scope(log);
    Log::Error("error");
    Log::Warn("warn");
    Log::Info("info");
    Log::Debug("debug");
  }

  const auto sink = std::make_shared<LevelSink>(Log::Level::Info);
  const SinkScope sinks(sink);
  log.Replay(begin, log.End());

  const std::vector<LevelSink::Line> expected{{Log::Level::Error, "error"},
                                              {Log::Level::Warn, "warn"},
                                              {Log::Level::Info, "info"}};
  CHECK(sink->Lines() == expected);
}

TEST_CASE(
    "a replay adds the conditions the capturing thread salvaged to the "
    "count of the replaying thread",
    "[log][capture]") {
  Detail::CapturedLog log;
  log.Reset(all_levels);
  Detail::CapturedLog::Mark begin;
  Detail::CapturedLog::Mark end;
  std::uint64_t counted_by_worker = 0;
  std::thread worker([&] {
    const Detail::CaptureScope scope(log);
    begin = log.End();
    Log::Recoverable(true, "salvaged {}", 1);
    // Strict mode rejects: nothing is salvaged, and nothing counts.
    Log::Recoverable(false, "rejected");
    Log::Recoverable(true, "salvaged {}", 2);
    end = log.End();
    counted_by_worker = Detail::RecoveredCount();
  });
  worker.join();
  CHECK(counted_by_worker >= 2);

  const std::uint64_t before = Detail::RecoveredCount();
  log.Replay(begin, end);

  CHECK(Detail::RecoveredCount() - before == 2);
}

TEST_CASE("a thread counts the conditions it salvages while it captures",
          "[log][capture]") {
  Detail::CapturedLog log;
  log.Reset(0);
  std::uint64_t counted = 0;
  std::thread worker([&] {
    const Detail::CaptureScope scope(log);
    const std::uint64_t before = Detail::RecoveredCount();
    // No level is kept, and the condition still counts.
    Log::Recoverable(true, "salvaged");
    counted = Detail::RecoveredCount() - before;
  });
  worker.join();

  CHECK(counted == 1);
}

TEST_CASE("capture scopes nest and give the thread back to the outer one",
          "[log][capture]") {
  static_cast<void>(Tests::TakeCapturedLog());
  Detail::CapturedLog outer;
  Detail::CapturedLog inner;
  outer.Reset(all_levels);
  inner.Reset(all_levels);
  const Detail::CapturedLog::Mark outer_begin = outer.End();
  const Detail::CapturedLog::Mark inner_begin = inner.End();
  {
    const Detail::CaptureScope outer_scope(outer);
    Log::Info("outer 1");
    {
      const Detail::CaptureScope inner_scope(inner);
      Log::Info("inner");
    }
    Log::Info("outer 2");
  }

  outer.Replay(outer_begin, outer.End());
  CHECK(Tests::TakeCapturedLog() == "outer 1\nouter 2\n");
  inner.Replay(inner_begin, inner.End());
  CHECK(Tests::TakeCapturedLog() == "inner\n");
}

TEST_CASE("a capture reset forgets its lines and takes new levels",
          "[log][capture]") {
  static_cast<void>(Tests::TakeCapturedLog());
  Detail::CapturedLog log;
  log.Reset(all_levels);
  {
    const Detail::CaptureScope scope(log);
    Log::Info("first");
  }
  log.Reset(Detail::LevelBit(Log::Level::Info));
  const Detail::CapturedLog::Mark begin = log.End();
  {
    const Detail::CaptureScope scope(log);
    Log::Info("second");
    Log::Warn("lost");
  }

  log.Replay(
      Detail::CapturedLog::Mark{.lines = 0, .recovered = begin.recovered},
      log.End());

  CHECK(Tests::TakeCapturedLog() == "second\n");
}

TEST_CASE("a replay past the end of the capture stops at its end",
          "[log][capture]") {
  static_cast<void>(Tests::TakeCapturedLog());
  Detail::CapturedLog log;
  log.Reset(all_levels);
  const Detail::CapturedLog::Mark begin = log.End();
  {
    const Detail::CaptureScope scope(log);
    Log::Info("only");
  }
  Detail::CapturedLog::Mark end = log.End();
  end.lines += 10;

  log.Replay(begin, end);

  CHECK(Tests::TakeCapturedLog() == "only\n");
}
