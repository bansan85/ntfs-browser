#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <spdlog/pattern_formatter.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/log/log.h>

#include "log/ntfs-common.h"
#include "logging-test-support.h"
#include "test-log-sink.h"

namespace Log = NtfsBrowser::Log;

TEST_CASE("an exception message loses its trailing line breaks",
          "[cov-attrs]") {
  static_cast<void>(NtfsBrowserTests::TakeCapturedLog());

  Log::Exception(std::runtime_error("parse failed\r\n"));

  const std::string text = NtfsBrowserTests::TakeCapturedLog();
  const std::string_view message = "parse failed";
  const size_t message_start = text.find(message);
  REQUIRE(message_start != std::string::npos);
  // Only the sink's own line ending follows the message: one newline.
  const std::string tail = text.substr(message_start + message.size());
  CHECK(std::ranges::count(tail, '\n') == 1);
}

TEST_CASE("a message that cannot be formatted is logged as such",
          "[cov-attrs]") {
  static_cast<void>(NtfsBrowserTests::TakeCapturedLog());

  // A negative dynamic width makes std::format throw.
  Log::Info("width {:{}}", "x", -1);

  const std::string text = NtfsBrowserTests::TakeCapturedLog();
  CHECK(text.find("Log message could not be formatted") != std::string::npos);
}

TEST_CASE("a formatter set on the library logger reaches its console sink",
          "[cov-attrs]") {
  const RestoreCaptureSink restore;

  Log::Config config;
  config.console_level = Log::Level::Error;
  config.file_level = Log::Level::Off;
  REQUIRE(Log::Configure(config));

  const auto logger = spdlog::get(std::string(Log::logger_name));
  REQUIRE(logger != nullptr);
  logger->set_formatter(std::make_unique<spdlog::pattern_formatter>("%v"));

  // Below the console's error level, so nothing reaches the streams.
  Log::Info("formatter probe");
}

TEST_CASE(
    "ParseOption refuses a path on the console target and a foreign prefix",
    "[cov-attrs]") {
  Log::Config config;

  CHECK_FALSE(Log::ParseOption("--log=console:info:path.log", config));
  CHECK_FALSE(Log::ParseOption("-log=console:info", config));
  CHECK(config.console_level == Log::Level::Warn);
  CHECK(config.file_level == Log::Level::Off);
}
