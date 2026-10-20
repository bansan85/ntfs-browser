#include <chrono>
#include <format>
#include <iostream>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>

#include "log/ntfs-common.h"
#include "logging-test-support.h"

namespace Log = NTFS_BROWSER_NAMESPACE::Log;

namespace {

// Calls per timing run. Large enough that the clock resolution and the
// first-call costs vanish in the total.
constexpr int rejected_calls = 10'000'000;

// Prints the mean cost of one Trace call that no sink accepts, with the
// library configured as config says.
void TimeRejectedCalls(const char* name, const Log::Config& config) {
  REQUIRE(Log::Configure(config));

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < rejected_calls; ++i) {
    Log::Trace("rejected {}", i);
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;

  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  std::cout << std::format("rejected log calls, {}: {:.2f} ns per call\n", name,
                           static_cast<double>(nanoseconds) / rejected_calls);
}

}  // namespace

TEST_CASE("rejected log calls cost", "[.benchmark]") {
  // Declared first, so it is removed after the restore has released the file
  // sink.
  const TempFile file(L"rejection-cost");
  const RestoreCaptureSink restore;

  Log::Config silent;
  silent.console_level = Log::Level::Off;
  silent.file_level = Log::Level::Off;
  TimeRejectedCalls("no target", silent);

  Log::Config console;
  console.console_level = Log::Level::Warn;
  console.file_level = Log::Level::Off;
  TimeRejectedCalls("console at warn", console);

  Log::Config both;
  both.console_level = Log::Level::Warn;
  both.file_level = Log::Level::Error;
  both.file_path = file.Path();
  TimeRejectedCalls("console at warn, file at error", both);
}
