#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/log/log.h>

#include "log/ntfs-common.h"
#include "logging-test-support.h"
#include "test-log-sink.h"

namespace Log = NtfsBrowser::Log;
using Catch::Matchers::ContainsSubstring;

namespace {

// A sink whose every line fails, the way a full disk or a closed file would.
class FailingSink final
    : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& /*msg*/) override {
    throw std::runtime_error("sink failure");
  }

  void flush_() override {}
};

// Adds a failing sink and an error handler that throws to the library logger,
// and takes both back out on destruction, even when a check fails first.
class FailingLoggerProbe final {
 public:
  explicit FailingLoggerProbe(std::shared_ptr<spdlog::logger> logger)
      : logger_(std::move(logger)) {
    logger_->sinks().push_back(std::make_shared<FailingSink>());
    logger_->set_error_handler([](const std::string& /*msg*/) {
      throw std::runtime_error("error handler failure");
    });
  }

  FailingLoggerProbe(FailingLoggerProbe&&) = delete;
  FailingLoggerProbe(const FailingLoggerProbe&) = delete;
  FailingLoggerProbe& operator=(FailingLoggerProbe&&) = delete;
  FailingLoggerProbe& operator=(const FailingLoggerProbe&) = delete;

  ~FailingLoggerProbe() {
    logger_->set_error_handler(nullptr);
    logger_->sinks().pop_back();
  }

 private:
  std::shared_ptr<spdlog::logger> logger_;
};

}  // namespace

TEST_CASE("a log call survives a sink and an error handler that both throw",
          "[logging]") {
  static_cast<void>(NtfsBrowserTests::TakeCapturedLog());

  const auto logger = spdlog::get(std::string(Log::logger_name));
  REQUIRE(logger != nullptr);
  const FailingLoggerProbe probe(logger);

  // The capturing sink sits before the failing one, so it still gets the line.
  REQUIRE_NOTHROW(Log::Info("line before the failing sink"));
  CHECK_THAT(NtfsBrowserTests::TakeCapturedLog(),
             ContainsSubstring("line before the failing sink"));
}

TEST_CASE("Level::Off reads as logged, even with no sink configured",
          "[logging]") {
  const RestoreCaptureSink restore;

  Log::Config config;
  config.console_level = Log::Level::Off;
  config.file_level = Log::Level::Off;
  REQUIRE(Log::Configure(config));

  // Off ranks above every message level, so the logger's own level test
  // passes it. A message level that no sink takes reads as not logged.
  CHECK_FALSE(Log::IsLogged(Log::Level::Info));
  CHECK(Log::IsLogged(Log::Level::Off));
}
