// Tests for the opt-in spdlog adapters
// (include/ntfs-browser/log/spdlog-adapter.h). The adapter header comes
// first so that it has to include everything it uses itself.

#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/log/spdlog-adapter.h>
#include <ntfs-browser/namespace.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "log/ntfs-common.h"
#include "logging-test-support.h"

namespace Log = NTFS_BROWSER_NAMESPACE::Log;
using Catch::Matchers::ContainsSubstring;

namespace {

// A spdlog sink that writes into a string stream, one "level text" line per
// message.
struct StreamSink {
  std::ostringstream out;
  std::shared_ptr<spdlog::sinks::ostream_sink_st> sink =
      std::make_shared<spdlog::sinks::ostream_sink_st>(out);

  StreamSink() { sink->set_pattern("%l %v"); }
};

}  // namespace

TEST_CASE("MakeSpdlogSink forwards a message with its level",
          "[logging][spdlog-adapter]") {
  const RestoreCaptureSink restore;
  const StreamSink stream;
  Log::SetSinks({Log::MakeSpdlogSink(stream.sink)});

  Log::Warn("careful {}", 1);
  Log::Debug("detail");

  CHECK_THAT(stream.out.str(), ContainsSubstring("warning careful 1"));
  CHECK_THAT(stream.out.str(), ContainsSubstring("debug detail"));
}

TEST_CASE("MakeSpdlogSink names the library logger",
          "[logging][spdlog-adapter]") {
  const RestoreCaptureSink restore;
  const StreamSink stream;
  stream.sink->set_pattern("%n %v");
  Log::SetSinks({Log::MakeSpdlogSink(stream.sink)});

  Log::Error("named");

  CHECK_THAT(stream.out.str(), ContainsSubstring("ntfs-browser named"));
}

TEST_CASE("MakeSpdlogSink honours its own threshold",
          "[logging][spdlog-adapter]") {
  const RestoreCaptureSink restore;
  const StreamSink stream;
  Log::SetSinks({Log::MakeSpdlogSink(stream.sink, Log::Level::Warn)});

  Log::Info("below the threshold");
  Log::Error("at or above it");

  CHECK_THAT(stream.out.str(), !ContainsSubstring("below the threshold"));
  CHECK_THAT(stream.out.str(), ContainsSubstring("error at or above it"));
}

TEST_CASE("MakeSpdlogSink honours the level of the spdlog sink",
          "[logging][spdlog-adapter]") {
  const RestoreCaptureSink restore;
  const StreamSink stream;
  stream.sink->set_level(spdlog::level::err);
  Log::SetSinks({Log::MakeSpdlogSink(stream.sink)});

  CHECK_FALSE(Log::IsLogged(Log::Level::Warn));
  CHECK(Log::IsLogged(Log::Level::Error));
}

TEST_CASE("MakeSpdlogSink and MakeSpdlogLogger return null for null",
          "[logging][spdlog-adapter]") {
  CHECK(Log::MakeSpdlogSink(nullptr) == nullptr);
  CHECK(Log::MakeSpdlogLogger(nullptr) == nullptr);
}

TEST_CASE("MakeSpdlogLogger emits through the logger of the consumer",
          "[logging][spdlog-adapter]") {
  const RestoreCaptureSink restore;
  const StreamSink stream;
  const auto logger = std::make_shared<spdlog::logger>("consumer", stream.sink);
  logger->set_level(spdlog::level::trace);
  Log::SetSinks({Log::MakeSpdlogLogger(logger)});

  Log::Info("through the consumer logger");

  CHECK_THAT(stream.out.str(),
             ContainsSubstring("info through the consumer logger"));
}

TEST_CASE("MakeSpdlogLogger respects the level of the logger",
          "[logging][spdlog-adapter]") {
  const RestoreCaptureSink restore;
  const StreamSink stream;
  const auto logger = std::make_shared<spdlog::logger>("consumer", stream.sink);
  logger->set_level(spdlog::level::err);
  Log::SetSinks({Log::MakeSpdlogLogger(logger)});

  CHECK_FALSE(Log::IsLogged(Log::Level::Warn));
  Log::Warn("dropped");

  CHECK(stream.out.str().empty());
}

TEST_CASE("MakeSpdlogLogger follows a level the consumer changes later",
          "[logging][spdlog-adapter]") {
  const RestoreCaptureSink restore;
  const StreamSink stream;
  const auto logger = std::make_shared<spdlog::logger>("consumer", stream.sink);
  logger->set_level(spdlog::level::err);
  Log::SetSinks({Log::MakeSpdlogLogger(logger)});
  CHECK_FALSE(Log::IsLogged(Log::Level::Info));

  logger->set_level(spdlog::level::info);

  CHECK(Log::IsLogged(Log::Level::Info));
}

TEST_CASE("MakeSpdlogLogger keeps the logger alive after the consumer lets go",
          "[logging][spdlog-adapter]") {
  const RestoreCaptureSink restore;
  const StreamSink stream;
  std::weak_ptr<spdlog::logger> weak;
  {
    auto logger = std::make_shared<spdlog::logger>("consumer", stream.sink);
    logger->set_level(spdlog::level::trace);
    weak = logger;
    Log::SetSinks({Log::MakeSpdlogLogger(std::move(logger))});
  }

  CHECK_FALSE(weak.expired());
  Log::Info("still delivered");
  CHECK_THAT(stream.out.str(), ContainsSubstring("info still delivered"));

  Log::SetSinks({});
  CHECK(weak.expired());
}

TEST_CASE("a consumer logger and the built-in file sink work together",
          "[logging][spdlog-adapter]") {
  const RestoreCaptureSink restore;
  const TempFile file(L"adapter");
  const StreamSink stream;
  const auto logger = std::make_shared<spdlog::logger>("consumer", stream.sink);
  logger->set_level(spdlog::level::trace);
  const std::shared_ptr<Log::ISink> file_sink =
      Log::MakeFileSink(file.Path(), Log::Level::Warn);
  REQUIRE(file_sink);
  Log::SetSinks({file_sink, Log::MakeSpdlogLogger(logger)});

  Log::Warn("to both");

  CHECK_THAT(stream.out.str(), ContainsSubstring("warning to both"));
  CHECK_THAT(file.Read(), ContainsSubstring("[warning] to both"));
}
