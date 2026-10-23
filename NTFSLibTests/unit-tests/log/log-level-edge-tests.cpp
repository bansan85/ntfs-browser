#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/sink.h>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/log/sink.h>
#include <ntfs-browser/log/spdlog-adapter.h>
#include <ntfs-browser/namespace.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "log/ntfs-common.h"
#include "logging-test-support.h"
#include "test-log-sink.h"

namespace Log = NTFS_BROWSER_NAMESPACE::Log;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("a file sink writes a debug line under the debug label",
          "[logging][levels]") {
  const RestoreCaptureSink restore;
  const TempFile log_file(L"debug");

  std::shared_ptr<Log::ISink> sink =
      Log::MakeFileSink(log_file.Path(), Log::Level::Debug);
  REQUIRE(sink != nullptr);
  Log::SetSinks({sink});

  Log::Debug("debug-line");
  Log::Trace("trace-line");

  // Drops the file sink, which closes the file before it is read back.
  Log::SetSinks({});
  sink.reset();
  CHECK_THAT(log_file.Read(), ContainsSubstring("[debug] debug-line"));
  CHECK_THAT(log_file.Read(), !ContainsSubstring("trace-line"));
}

TEST_CASE("a line written to a sink with the off level carries that label",
          "[logging][levels]") {
  const RestoreCaptureSink restore;
  const TempFile log_file(L"off-level");
  std::shared_ptr<Log::ISink> built_in =
      Log::MakeFileSink(log_file.Path(), Log::Level::Trace);
  REQUIRE(built_in != nullptr);

  std::ostringstream out;
  const auto stream = std::make_shared<spdlog::sinks::ostream_sink_st>(out);
  const std::shared_ptr<Log::ISink> adapted = Log::MakeSpdlogSink(stream);

  CHECK_FALSE(built_in->ShouldLog(Log::Level::Off));
  CHECK_FALSE(adapted->ShouldLog(Log::Level::Off));

  // Off is no message level, and a sink does not filter what it is handed.
  built_in->Write(Log::Level::Off, "off-line");
  adapted->Write(Log::Level::Off, "off-line");
  built_in->Flush();
  adapted->Flush();

  built_in.reset();
  CHECK_THAT(out.str(), ContainsSubstring("off-line"));
  CHECK_THAT(log_file.Read(), ContainsSubstring("[off] off-line"));
}

TEST_CASE("a silenced thread emits nothing", "[logging][levels]") {
  static_cast<void>(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog());

  std::thread worker([] {
    Log::Detail::SilenceCurrentThread();
    // Emit() itself drops the line, even when called without IsEnabled().
    Log::Detail::Emit(Log::Level::Error, "silenced-line");
    Log::Error("silenced-line");
  });
  worker.join();

  CHECK_THAT(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog(),
             !ContainsSubstring("silenced-line"));
}
