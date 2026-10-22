#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "child-process.h"

namespace Fs = std::filesystem;

namespace {

// An executable that does not exist, in the temp directory.
Fs::path MissingExecutable() {
  return Fs::temp_directory_path() / "ntfsbrowser-missing-child-exe.exe";
}

// A redirect file under a directory that does not exist, so it cannot be
// created.
Fs::path UncreatableRedirect() {
  return Fs::temp_directory_path() / "ntfsbrowser-missing-dir" / "child.txt";
}

std::string ReadWholeFile(const Fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {(std::istreambuf_iterator<char>(input)),
          std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("RunProcessToFiles throws when the executable is missing",
          "[child-process]") {
  const Fs::path out =
      Fs::temp_directory_path() / "ntfsbrowser-child-missing-out.txt";
  CHECK_THROWS_AS(NTFS_BROWSER_NAMESPACE::Tests::RunProcessToFiles(
                      MissingExecutable(), {}, out, out),
                  std::runtime_error);
}

TEST_CASE("RunProcessToFiles throws when a redirect file cannot be created",
          "[child-process]") {
  CHECK_THROWS_AS(NTFS_BROWSER_NAMESPACE::Tests::RunProcessToFiles(
                      NTFS_FUZZER_AFL_EXE, {}, UncreatableRedirect(),
                      UncreatableRedirect()),
                  std::runtime_error);
}

TEST_CASE(
    "RunProcessToFiles returns the exit code and writes stderr to its file",
    "[child-process]") {
  const Fs::path out =
      Fs::temp_directory_path() / "ntfsbrowser-child-usage-out.txt";
  const Fs::path err =
      Fs::temp_directory_path() / "ntfsbrowser-child-usage-err.txt";
  const int exit_code = NTFS_BROWSER_NAMESPACE::Tests::RunProcessToFiles(
      NTFS_FUZZER_AFL_EXE, {}, out, err);
  CHECK(exit_code == 1);
  CHECK_THAT(ReadWholeFile(err), Catch::Matchers::ContainsSubstring("usage:"));
}
