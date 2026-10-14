#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "sample-run.h"

using Catch::Matchers::ContainsSubstring;
using NtfsBrowserTests::RunSample;
using NtfsBrowserTests::SampleRun;
namespace Fs = std::filesystem;

namespace {

// Runs NtfsCompare with args, its stdout and stderr captured apart.
SampleRun RunCompare(const std::vector<std::wstring>& args) {
  const Fs::path exe(NTFS_COMPARE_EXE);
  REQUIRE(Fs::exists(exe));
  return RunSample(exe, args);
}

}  // namespace

TEST_CASE("NtfsCompare prints usage without arguments",
          "[ntfscompare][sample][characterization]") {
  const SampleRun run = RunCompare({});

  CHECK(run.exit_code == 1);
  CHECK_THAT(run.err, ContainsSubstring("usage: "));
  CHECK_THAT(run.err, ContainsSubstring("<folder>"));
  CHECK(run.out.empty());
}

TEST_CASE("NtfsCompare prints usage for two folders",
          "[ntfscompare][sample][characterization]") {
  const SampleRun run = RunCompare({L"first", L"second"});

  CHECK(run.exit_code == 1);
  CHECK_THAT(run.err, ContainsSubstring("usage: "));
  CHECK(run.out.empty());
}

TEST_CASE("NtfsCompare prints usage for a malformed --log option",
          "[ntfscompare][sample][characterization]") {
  const SampleRun run = RunCompare({L"--log=bogus", L"folder"});

  CHECK(run.exit_code == 1);
  CHECK_THAT(run.err, ContainsSubstring("usage: "));
  CHECK(run.out.empty());
}

TEST_CASE("NtfsCompare refuses a missing folder before opening any volume",
          "[ntfscompare][sample][characterization]") {
  const SampleRun run = RunCompare({L"ntfscompare-no-such-folder"});

  CHECK(run.exit_code == 1);
  CHECK(run.err == "ntfscompare-no-such-folder is not a directory\n");
  CHECK(run.out.empty());
}

TEST_CASE("NtfsCompare refuses a file before opening any volume",
          "[ntfscompare][sample][characterization]") {
  std::random_device random_device;
  const Fs::path file =
      Fs::temp_directory_path() /
      ("ntfscompare-sample-" + std::to_string(random_device()) + ".txt");
  std::ofstream(file, std::ios::binary) << "not a folder";

  const SampleRun run = RunCompare({file.wstring()});

  std::error_code ignored;
  Fs::remove(file, ignored);
  CHECK(run.exit_code == 1);
  CHECK(run.err == file.string() + " is not a directory\n");
  CHECK(run.out.empty());
}
