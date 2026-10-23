// Tests for the sinks a consumer installs (include/ntfs-browser/log/sink.h):
// delivery, thresholds, replacement, and a sink that fails.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/namespace.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "log/ntfs-common.h"
#include "logging-test-support.h"
#include "test-log-sink.h"

namespace Log = NTFS_BROWSER_NAMESPACE::Log;
using Catch::Matchers::ContainsSubstring;

namespace {

// Keeps what it is given, and answers ShouldLog() from its threshold the
// way a built-in target does.
class RecordingSink final : public Log::ISink {
 public:
  using Line = std::pair<Log::Level, std::string>;

  explicit RecordingSink(Log::Level threshold) : threshold_(threshold) {}

  [[nodiscard]] bool ShouldLog(Log::Level level) const override {
    ++asked_;
    return level != Log::Level::Off && level <= threshold_;
  }

  void Write(Log::Level level, std::string_view message) override {
    lines_.emplace_back(level, std::string(message));
  }

  void Flush() override { ++flushes_; }

  [[nodiscard]] const std::vector<Line>& Lines() const { return lines_; }

  [[nodiscard]] std::size_t Flushes() const { return flushes_; }

  [[nodiscard]] std::size_t Asked() const { return asked_; }

 private:
  Log::Level threshold_;
  std::vector<Line> lines_;
  std::size_t flushes_ = 0;
  mutable std::size_t asked_ = 0;
};

// A sink that fails in the member its flags name.
class ThrowingSink final : public Log::ISink {
 public:
  ThrowingSink(bool throws_in_should_log, bool throws_in_write)
      : throws_in_should_log_(throws_in_should_log),
        throws_in_write_(throws_in_write) {}

  [[nodiscard]] bool ShouldLog(Log::Level /*level*/) const override {
    if (throws_in_should_log_) {
      throw std::runtime_error("ShouldLog failure");
    }
    return true;
  }

  void Write(Log::Level /*level*/, std::string_view /*message*/) override {
    if (throws_in_write_) {
      throw std::runtime_error("Write failure");
    }
  }

  void Flush() override {}

 private:
  bool throws_in_should_log_;
  bool throws_in_write_;
};

// A sink that replaces the library's whole sink set from inside Write(),
// the way a sink that gives up after an error would. It is the only owner
// of itself once the set is installed.
class ReplacingSink final : public Log::ISink {
 public:
  [[nodiscard]] bool ShouldLog(Log::Level /*level*/) const override {
    return true;
  }

  void Write(Log::Level /*level*/, std::string_view /*message*/) override {
    Log::SetSinks({});
    // Touches a member after the set is gone: it MUST still be alive.
    ++writes_;
  }

  void Flush() override { ++flushes_; }

 private:
  std::size_t writes_ = 0;
  std::size_t flushes_ = 0;
};

}  // namespace

TEST_CASE("a sink may replace the sinks from inside Write",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const auto after = std::make_shared<RecordingSink>(Log::Level::Trace);
  Log::SetSinks({std::make_shared<ReplacingSink>(), after});

  REQUIRE_NOTHROW(Log::Error("first message"));
  // The message in flight reaches the rest of the old set.
  CHECK(after->Lines().size() == 1);

  // The new set is empty, so the next message goes nowhere.
  Log::Error("second message");
  CHECK(after->Lines().size() == 1);
}

TEST_CASE("SetSinks delivers only the messages a sink's threshold accepts",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const auto sink = std::make_shared<RecordingSink>(Log::Level::Info);
  Log::SetSinks({sink});

  Log::Debug("too detailed");
  Log::Warn("kept {}", 1);

  REQUIRE(sink->Lines().size() == 1);
  CHECK(sink->Lines().front().first == Log::Level::Warn);
  CHECK(sink->Lines().front().second == "kept 1");
  CHECK(sink->Flushes() == 1);
}

TEST_CASE("SetSinks offers a message to every sink that accepts it once",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const auto quiet = std::make_shared<RecordingSink>(Log::Level::Error);
  const auto middle = std::make_shared<RecordingSink>(Log::Level::Info);
  const auto verbose = std::make_shared<RecordingSink>(Log::Level::Trace);
  Log::SetSinks({quiet, middle, verbose});

  Log::Info("info line");

  CHECK(quiet->Lines().empty());
  CHECK(middle->Lines().size() == 1);
  CHECK(verbose->Lines().size() == 1);
}

TEST_CASE("SetSinks replaces the sinks of the previous call",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const auto first = std::make_shared<RecordingSink>(Log::Level::Trace);
  const auto second = std::make_shared<RecordingSink>(Log::Level::Trace);

  Log::SetSinks({first});
  Log::Info("to the first");
  Log::SetSinks({second});
  Log::Info("to the second");

  REQUIRE(first->Lines().size() == 1);
  REQUIRE(second->Lines().size() == 1);
  CHECK(second->Lines().front().second == "to the second");
}

TEST_CASE("SetSinks skips a null entry", "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const auto sink = std::make_shared<RecordingSink>(Log::Level::Trace);
  Log::SetSinks({nullptr, sink, nullptr});

  REQUIRE_NOTHROW(Log::Info("past the nulls"));
  CHECK(sink->Lines().size() == 1);
}

TEST_CASE("SetSinks with no sink discards every message", "[logging][sinks]") {
  const RestoreCaptureSink restore;
  Log::SetSinks({});

  CHECK_FALSE(Log::IsLogged(Log::Level::Error));
  REQUIRE_NOTHROW(Log::Error("nowhere to go"));

  static_cast<void>(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog());
}

TEST_CASE("SetSinks keeps a sink alive after the caller let go of it",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  std::weak_ptr<RecordingSink> weak;
  {
    const auto sink = std::make_shared<RecordingSink>(Log::Level::Trace);
    weak = sink;
    Log::SetSinks({sink});
  }

  CHECK_FALSE(weak.expired());
  REQUIRE_NOTHROW(Log::Info("still delivered"));
  {
    const auto sink = weak.lock();
    REQUIRE(sink);
    CHECK(sink->Lines().size() == 1);
  }

  Log::SetSinks({});
  CHECK(weak.expired());
}

TEST_CASE("a sink that throws in Write never makes a log call throw",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const auto after = std::make_shared<RecordingSink>(Log::Level::Trace);
  Log::SetSinks({std::make_shared<ThrowingSink>(false, true), after});

  REQUIRE_NOTHROW(Log::Error("past the failure"));
  REQUIRE(after->Lines().size() == 1);
  CHECK(after->Lines().front().second == "past the failure");
}

TEST_CASE("a sink that throws in ShouldLog is contained", "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const auto after = std::make_shared<RecordingSink>(Log::Level::Trace);
  Log::SetSinks({std::make_shared<ThrowingSink>(true, false), after});

  REQUIRE_NOTHROW(Log::Info("past the failure"));
  CHECK(after->Lines().size() == 1);
  CHECK(Log::IsLogged(Log::Level::Info));

  Log::SetSinks({std::make_shared<ThrowingSink>(true, false)});
  CHECK_FALSE(Log::IsLogged(Log::Level::Info));
}

TEST_CASE("a sink that wants nothing is never asked to write",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const auto off = std::make_shared<RecordingSink>(Log::Level::Off);
  Log::SetSinks({off});

  CHECK_FALSE(Log::IsLogged(Log::Level::Trace));
  Log::Trace("rejected");

  CHECK(off->Lines().empty());
  CHECK(off->Flushes() == 0);
  CHECK(off->Asked() >= 1);
}

TEST_CASE("the built-in file sink sits next to a sink of the caller",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const TempFile file(L"custom-sink");
  const auto recording = std::make_shared<RecordingSink>(Log::Level::Trace);

  const std::shared_ptr<Log::ISink> file_sink =
      Log::MakeFileSink(file.Path(), Log::Level::Info);
  REQUIRE(file_sink);
  Log::SetSinks({file_sink, recording});

  Log::Info("both destinations");
  Log::Debug("recording only");

  CHECK(recording->Lines().size() == 2);
  const std::string text = file.Read();
  CHECK_THAT(text, ContainsSubstring("[info] both destinations"));
  CHECK_THAT(text, !ContainsSubstring("recording only"));
}

TEST_CASE("MakeFileSink returns null for a file it cannot open",
          "[logging][sinks]") {
  // A directory cannot be opened as a file. A missing directory would not
  // do: the file sink creates it.
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path();

  CHECK(Log::MakeFileSink(directory, Log::Level::Info) == nullptr);
}

TEST_CASE("MakeConsoleSink honours its threshold", "[logging][sinks]") {
  const std::shared_ptr<Log::ISink> console =
      Log::MakeConsoleSink(Log::Level::Warn);
  REQUIRE(console);

  CHECK(console->ShouldLog(Log::Level::Error));
  CHECK(console->ShouldLog(Log::Level::Warn));
  CHECK_FALSE(console->ShouldLog(Log::Level::Info));
  CHECK_FALSE(console->ShouldLog(Log::Level::Off));
  CHECK_FALSE(
      Log::MakeConsoleSink(Log::Level::Off)->ShouldLog(Log::Level::Error));
}

TEST_CASE("Configure replaces the sinks SetSinks installed",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  const auto sink = std::make_shared<RecordingSink>(Log::Level::Trace);
  Log::SetSinks({sink});

  Log::Config config;
  config.console_level = Log::Level::Off;
  config.file_level = Log::Level::Off;
  REQUIRE(Log::Configure(config));
  Log::Error("after Configure");

  CHECK(sink->Lines().empty());
}

TEST_CASE("SetSinks replaces the targets Configure installed",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  Log::Config config;
  config.console_level = Log::Level::Off;
  config.file_level = Log::Level::Off;
  REQUIRE(Log::Configure(config));
  CHECK_FALSE(Log::IsLogged(Log::Level::Error));

  const auto sink = std::make_shared<RecordingSink>(Log::Level::Error);
  Log::SetSinks({sink});

  CHECK(Log::IsLogged(Log::Level::Error));
  Log::Error("after SetSinks");
  CHECK(sink->Lines().size() == 1);
}

TEST_CASE("Configure with a default Config brings back warn on the console",
          "[logging][sinks]") {
  const RestoreCaptureSink restore;
  Log::SetSinks({});
  CHECK_FALSE(Log::IsLogged(Log::Level::Warn));

  REQUIRE(Log::Configure(Log::Config{}));

  CHECK(Log::IsLogged(Log::Level::Warn));
  CHECK_FALSE(Log::IsLogged(Log::Level::Info));
}
