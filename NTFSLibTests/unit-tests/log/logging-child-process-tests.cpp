// The split cannot be observed in process: on Windows spdlog's console
// sinks cache the HANDLE that GetStdHandle() returned when they were
// built, so redirecting a CRT file descriptor changes nothing. The split
// is therefore checked by running NtfsFuzzerAfl - which drives the library
// over a saved corpus file and takes --log itself - with its two standard
// streams sent to two separate files.

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "catch2/matchers/catch_matchers.hpp"
#include "logging-test-support.h"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("the console target splits by level across the two streams",
          "[logging]") {
  SECTION("console:warn: warnings on stderr, stdout silent") {
    const ChildOutput result = RunFuzzer({L"--log=console:warn"});
    CHECK(result.exit_code == 0);
    CHECK_THAT(result.err, ContainsSubstring(std::string(error_line)));
    CHECK_THAT(result.out, !ContainsSubstring(std::string(error_line)));
    CHECK_THAT(result.out, !ContainsSubstring(std::string(info_line)));
  }

  SECTION("console:trace: each line on exactly one stream") {
    const ChildOutput result = RunFuzzer({L"--log=console:trace"});
    CHECK(result.exit_code == 0);
    CHECK_THAT(result.out, ContainsSubstring(std::string(info_line)));
    CHECK_THAT(result.out, !ContainsSubstring(std::string(error_line)));
    CHECK_THAT(result.err, ContainsSubstring(std::string(error_line)));
    CHECK_THAT(result.err, !ContainsSubstring(std::string(info_line)));
  }

  SECTION("console:error: stdout stays silent") {
    const ChildOutput result = RunFuzzer({L"--log=console:error"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.empty());
    CHECK_THAT(result.err, ContainsSubstring(std::string(error_line)));
  }

  SECTION("console:off: nothing on either stream") {
    const ChildOutput result = RunFuzzer({L"--log=console:off"});
    CHECK(result.exit_code == 0);
    CHECK(result.out.empty());
    CHECK(result.err.empty());
  }

  SECTION("an unknown target is rejected with a non-zero exit") {
    const ChildOutput result = RunFuzzer({L"--log=syslog:debug"});
    CHECK(result.exit_code != 0);
  }

  SECTION("an unknown level is rejected with a non-zero exit") {
    const ChildOutput result = RunFuzzer({L"--log=console:verbose"});
    CHECK(result.exit_code != 0);
  }
}

TEST_CASE("the file target records what the console target is denied",
          "[logging]") {
  const TempFile log_file(L"child");

  const ChildOutput result = RunFuzzer(
      {L"--log=console:off", L"--log=file:trace:" + log_file.Path().wstring()});
  CHECK(result.exit_code == 0);
  CHECK(result.out.empty());
  CHECK(result.err.empty());

  const std::string contents = log_file.Read();
  CHECK_THAT(contents, ContainsSubstring(std::string(info_line)));
  CHECK_THAT(contents, ContainsSubstring(std::string(error_line)));
}
