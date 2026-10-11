// Tests for the runtime logging configuration (include/ntfs-browser/log/log.h):
// the --log option parser, per-target levels, and the console target's
// split between stdout and stderr.

#include <array>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/log/log.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "log/ntfs-common.h"
#include "logging-test-support.h"
#include "test-log-sink.h"

namespace Fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using NtfsBrowser::Log::Config;
using NtfsBrowser::Log::Level;

TEST_CASE("the --log option parses a target and a level", "[logging]") {
  Config config;

  SECTION("console level") {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=console:debug", config));
    CHECK(config.console_level == Level::Debug);
    CHECK(config.file_level == Level::Off);
  }

  SECTION("file level, default path") {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=file:debug", config));
    CHECK(config.file_level == Level::Debug);
    CHECK(config.file_path == NtfsBrowser::Log::default_file_path);
    CHECK(config.console_level == Level::Warn);
  }

  SECTION("file level and path, drive letter kept") {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=file:trace:C:\\tmp\\ntfs.log",
                                          config));
    CHECK(config.file_level == Level::Trace);
    CHECK(config.file_path == "C:\\tmp\\ntfs.log");
  }

#ifdef _WIN32
  // The wide ParseOption() overload only exists for wmain()'s wide argv,
  // which only exists on Windows.
  SECTION("wide option, path kept as wide characters") {
    REQUIRE(NtfsBrowser::Log::ParseOption(
        L"--log=file:trace:C:\\tmp\\\u30ed.log", config));
    CHECK(config.file_level == Level::Trace);
    CHECK(config.file_path == Fs::path(L"C:\\tmp\\\u30ed.log"));
  }
#endif

  SECTION("repeated, once per target") {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=console:error", config));
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=file:trace", config));
    CHECK(config.console_level == Level::Error);
    CHECK(config.file_level == Level::Trace);
  }

  SECTION("either target may be off") {
    REQUIRE(NtfsBrowser::Log::ParseOption("--log=console:off", config));
    CHECK(config.console_level == Level::Off);
  }
}

TEST_CASE("the --log option rejects a malformed value and changes nothing",
          "[logging]") {
  const std::array<const char*, 6> rejected{
      "--log=syslog:debug", "--log=console:verbose", "--log=console", "--log=",
      "console:debug",      "--log=file:trace:"};

  for (const char* option : rejected) {
    Config config;
    INFO("option: " << option);
    CHECK_FALSE(NtfsBrowser::Log::ParseOption(option, config));
    CHECK(config.console_level == Level::Warn);
    CHECK(config.file_level == Level::Off);
    CHECK(config.file_path == NtfsBrowser::Log::default_file_path);
  }
}

TEST_CASE("the default configuration logs warnings, not info", "[logging]") {
  const Config config;
  CHECK(config.console_level == Level::Warn);
  CHECK(config.file_level == Level::Off);
  CHECK(config.file_path == NtfsBrowser::Log::default_file_path);
}

TEST_CASE("each sink keeps its own level", "[logging]") {
  const RestoreCaptureSink restore;
  const TempFile log_file(L"levels");

  Config config;
  config.console_level = Level::Off;
  config.file_level = Level::Warn;
  config.file_path = log_file.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));

  NtfsBrowser::Log::Trace("trace-only-line");
  NtfsBrowser::Log::Info("info-only-line");
  NtfsBrowser::Log::Warn("warn-line");
  NtfsBrowser::Log::Error("error-line");

  // Drops the file sink, which closes the file before it is read back.
  NtfsBrowserTests::InstallCaptureSink();

  const std::string contents = log_file.Read();
  CHECK_THAT(contents, ContainsSubstring("warn-line"));
  CHECK_THAT(contents, ContainsSubstring("error-line"));
  CHECK_THAT(contents, !ContainsSubstring("trace-only-line"));
  CHECK_THAT(contents, !ContainsSubstring("info-only-line"));
}

TEST_CASE("a file sink at trace records every level", "[logging]") {
  const RestoreCaptureSink restore;
  const TempFile log_file(L"trace");

  Config config;
  config.console_level = Level::Off;
  config.file_level = Level::Trace;
  config.file_path = log_file.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));

  NtfsBrowser::Log::Trace("recorded-trace");
  NtfsBrowser::Log::Error("recorded-error");

  NtfsBrowserTests::InstallCaptureSink();

  const std::string contents = log_file.Read();
  CHECK_THAT(contents, ContainsSubstring("recorded-trace"));
  CHECK_THAT(contents, ContainsSubstring("recorded-error"));
}

TEST_CASE("both targets off writes nothing at all", "[logging]") {
  const RestoreCaptureSink restore;
  const TempFile log_file(L"silent");

  Config config;
  config.console_level = Level::Off;
  config.file_level = Level::Off;
  config.file_path = log_file.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));

  NtfsBrowser::Log::Error("never-written");

  NtfsBrowserTests::InstallCaptureSink();
  CHECK_FALSE(Fs::exists(log_file.Path()));
}

TEST_CASE("a message carrying braces is not treated as a format string",
          "[logging]") {
  const RestoreCaptureSink restore;
  const TempFile log_file(L"braces");

  Config config;
  config.console_level = Level::Off;
  config.file_level = Level::Trace;
  config.file_path = log_file.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));

  // What Log::Exception() relays: runtime text, never a format string.
  const std::runtime_error thrown("relayed {0} {bad} text\n");
  NtfsBrowser::Log::Exception(thrown);

  NtfsBrowserTests::InstallCaptureSink();

  // Carriage returns go first: spdlog ends a line with \r\n here, which
  // would hide the thrower's own newline from the check below.
  std::string contents = log_file.Read();
  std::erase(contents, '\r');

  CHECK_THAT(contents, ContainsSubstring("relayed {0} {bad} text\n"));
  // Leaving the thrower's newline in would put a blank line after it.
  CHECK_THAT(contents, !ContainsSubstring("text\n\n"));
}

TEST_CASE("Configure() on an unwritable path fails without throwing",
          "[logging]") {
  const RestoreCaptureSink restore;

  Config config;
  config.console_level = Level::Warn;
  config.file_level = Level::Trace;
  // A parent directory that cannot exist, so the sink's fopen() must fail.
#ifdef _WIN32
  config.file_path = "Z:\\ntfs-browser-no-such-directory\\log.txt";
#else
  config.file_path = "/ntfs-browser-no-such-directory/log.txt";
#endif

  CHECK_FALSE(NtfsBrowser::Log::Configure(config));

  // The console target survives the file sink's failure.
  NtfsBrowser::Log::Error("still-logging");
}

#ifdef _WIN32
// The ANSI code page is a Windows concept, and the wide ParseOption()
// overload this exercises only exists there.
TEST_CASE("a log path outside the ANSI code page still opens", "[logging]") {
  const RestoreCaptureSink restore;
  // Japanese kana, which no Western Windows ANSI code page can express.
  // The file only opens if the path stays wide from --log through to the
  // sink's fopen().
  const TempFile log_file(L"\u30ed\u30b0");

  Config config;
  REQUIRE(NtfsBrowser::Log::ParseOption(
      std::wstring(L"--log=file:trace:") + log_file.Path().wstring(), config));
  CHECK(config.file_path == log_file.Path());

  config.console_level = Level::Off;
  REQUIRE(NtfsBrowser::Log::Configure(config));

  NtfsBrowser::Log::Error("wide-path-line");

  NtfsBrowserTests::InstallCaptureSink();

  CHECK(Fs::exists(log_file.Path()));
  CHECK_THAT(log_file.Read(), ContainsSubstring("wide-path-line"));
}
#endif

TEST_CASE("Configure() replaces the previous sinks wholesale", "[logging]") {
  const RestoreCaptureSink restore;
  const TempFile first(L"first");
  const TempFile second(L"second");

  Config config;
  config.console_level = Level::Off;
  config.file_level = Level::Trace;
  config.file_path = first.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));
  NtfsBrowser::Log::Error("into-first");

  config.file_path = second.Path();
  REQUIRE(NtfsBrowser::Log::Configure(config));
  NtfsBrowser::Log::Error("into-second");

  NtfsBrowserTests::InstallCaptureSink();

  CHECK_THAT(first.Read(), ContainsSubstring("into-first"));
  CHECK_THAT(first.Read(), !ContainsSubstring("into-second"));
  CHECK_THAT(second.Read(), ContainsSubstring("into-second"));
}
