#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "fuzz-core.h"
#include "test-log-sink.h"

namespace {

namespace Fs = std::filesystem;
using NTFS_BROWSER_NAMESPACE::Tests::InstallCaptureSink;

// The argv string type of this platform, as RunCommandLine() takes it.
using Arg = std::basic_string<NtfsFuzz::ArgChar>;

// An argument of this platform's argv type, from ASCII text.
Arg ArgOf(std::string_view text) { return Fs::path(text).native(); }

// What RunFuzzerInProcess() returned and wrote to its error stream.
struct CommandLineResult {
  int exit_code = 0;
  std::string err;
};

// Runs the command line in this process. Log::Configure() replaces the
// logger's sinks, so the capture sink is installed again afterwards.
CommandLineResult RunFuzzerInProcess(const std::vector<Arg>& args) {
  std::ostringstream err;
  CommandLineResult result;
  result.exit_code = NtfsFuzz::RunCommandLine(args, err);
  InstallCaptureSink();
  result.err = err.str();
  return result;
}

}  // namespace

TEST_CASE("NtfsFuzzerAfl command line", "[fuzz][options]") {
  const Fs::path testcase = Fs::path(NTFS_FUZZ_DATA_DIR) / "cluster_size_null";
  REQUIRE(Fs::exists(testcase));
  const Arg program = ArgOf("NtfsFuzzerAfl");

  SECTION("no input file prints usage and fails") {
    const CommandLineResult result = RunFuzzerInProcess({program});
    CHECK(result.exit_code == 1);
    CHECK_THAT(result.err, Catch::Matchers::ContainsSubstring("usage:"));
  }

  SECTION("a second input file prints usage and fails") {
    const CommandLineResult result =
        RunFuzzerInProcess({program, testcase.native(), testcase.native()});
    CHECK(result.exit_code == 1);
    CHECK_THAT(result.err, Catch::Matchers::ContainsSubstring("usage:"));
  }

  SECTION("an input that cannot be read is not an error") {
    const CommandLineResult result = RunFuzzerInProcess(
        {program, ArgOf("ntfsbrowser-no-such-testcase.bin")});
    CHECK(result.exit_code == 0);
  }

  SECTION("an input path with a space stays one argument") {
    const CommandLineResult result =
        RunFuzzerInProcess({program, ArgOf("no such testcase.bin")});
    CHECK(result.exit_code == 0);
  }

  SECTION(
      "a log file that cannot be created is reported, and the run goes on") {
    // The testcase is a file, so no directory can be created under it.
    const Fs::path log = testcase / "fuzzer.log";
    // The console is silenced, so the run's messages stay out of the output.
    const CommandLineResult result = RunFuzzerInProcess(
        {program, testcase.native(), ArgOf("--log=console:off"),
         ArgOf("--log=file:debug:") + log.native()});
    CHECK(result.exit_code == 0);
    CHECK_THAT(result.err,
               Catch::Matchers::ContainsSubstring("Cannot open log file"));
  }
}
