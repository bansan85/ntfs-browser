#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "child-process.h"
#include "test-log-sink.h"

namespace Fs = std::filesystem;

namespace {

// Corpus testcase driven by the child-process cases below. Its run logs an
// info line (the sector size) and an error line (the null cluster size),
// so one run exercises both halves of the console split.
constexpr std::string_view split_testcase = "cluster_size_null";
// Substrings those two lines are recognised by.
constexpr std::string_view info_line = "Sector Size = ";
constexpr std::string_view error_line = "Cluster Size can't be null";

// Puts the trace-level capturing sink back once a test has replaced the
// library logger's sinks with a configuration of its own.
class RestoreCaptureSink final {
 public:
  RestoreCaptureSink() = default;
  RestoreCaptureSink(RestoreCaptureSink&&) = delete;
  RestoreCaptureSink(const RestoreCaptureSink&) = delete;
  RestoreCaptureSink& operator=(RestoreCaptureSink&&) = delete;
  RestoreCaptureSink& operator=(const RestoreCaptureSink&) = delete;

  ~RestoreCaptureSink() { NTFS_BROWSER_NAMESPACE::Tests::InstallCaptureSink(); }
};

// A path in the temp directory that no other test uses, removed again by
// the destructor.
class TempFile final {
 public:
  explicit TempFile(std::wstring_view tag)
      : path_(Fs::temp_directory_path() /
              (L"ntfsbrowser-log-" + std::wstring(tag) + L"-" +
               std::to_wstring(std::random_device{}()) + L".txt")) {
    std::error_code error_code;
    Fs::remove(path_, error_code);
  }

  TempFile(TempFile&&) = delete;
  TempFile(const TempFile&) = delete;
  TempFile& operator=(TempFile&&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  ~TempFile() {
    std::error_code error_code;
    Fs::remove(path_, error_code);
  }

  [[nodiscard]] const Fs::path& Path() const noexcept { return path_; }

  [[nodiscard]] std::string Read() const {
    std::ifstream input(path_, std::ios::binary);
    return {(std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>()};
  }

 private:
  Fs::path path_;
};

struct ChildOutput {
  int exit_code = 0;
  std::string out;
  std::string err;
};

// Runs NtfsFuzzerAfl on the corpus testcase with extraArgs appended, and
// returns its two standard streams separately. Files rather than a pipe, so
// neither stream can fill a pipe buffer and deadlock the other.
inline ChildOutput RunFuzzer(const std::vector<std::wstring>& extra_args) {
  const Fs::path exe(NTFS_FUZZER_AFL_EXE);
  const Fs::path testcase =
      Fs::path(NTFS_FUZZ_DATA_DIR) / std::string(split_testcase);
  REQUIRE(Fs::exists(exe));
  REQUIRE(Fs::exists(testcase));

  const TempFile out_file(L"stdout");
  const TempFile err_file(L"stderr");

  std::vector<std::wstring> args{testcase.wstring()};
  args.insert(args.end(), extra_args.begin(), extra_args.end());

  ChildOutput result;
  result.exit_code = NTFS_BROWSER_NAMESPACE::Tests::RunProcessToFiles(
      exe, args, out_file.Path(), err_file.Path());
  result.out = out_file.Read();
  result.err = err_file.Read();
  return result;
}

}  // namespace
