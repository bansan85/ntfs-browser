#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

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

// A sink whose every line fails, the way a full disk or a closed file would.
class FailingSink final : public Log::ISink {
 public:
  [[nodiscard]] bool ShouldLog(Log::Level /*level*/) const override {
    return true;
  }

  void Write(Log::Level /*level*/, std::string_view /*message*/) override {
    throw std::runtime_error("sink failure");
  }

  void Flush() override { throw std::runtime_error("flush failure"); }
};

}  // namespace

TEST_CASE("a log call survives a sink that throws", "[logging]") {
  const RestoreCaptureSink restore;
  static_cast<void>(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog());

  // The capturing sink sits before the failing one, so it still gets the line.
  Log::SetSinks({NTFS_BROWSER_NAMESPACE::Tests::CaptureSinkHandle(),
                 std::make_shared<FailingSink>()});

  REQUIRE_NOTHROW(Log::Info("line before the failing sink"));
  CHECK_THAT(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog(),
             ContainsSubstring("line before the failing sink"));
}

TEST_CASE("Level::Off never reads as logged, with or without a sink",
          "[logging]") {
  const RestoreCaptureSink restore;

  Log::Config config;
  config.console_level = Log::Level::Off;
  config.file_level = Log::Level::Off;
  REQUIRE(Log::Configure(config));
  CHECK_FALSE(Log::IsLogged(Log::Level::Info));
  CHECK_FALSE(Log::IsLogged(Log::Level::Off));

  config.console_level = Log::Level::Info;
  REQUIRE(Log::Configure(config));
  CHECK(Log::IsLogged(Log::Level::Info));
  CHECK_FALSE(Log::IsLogged(Log::Level::Off));
}
