#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <windows.h>

#include "sample-run.h"

using Catch::Matchers::ContainsSubstring;
using NtfsBrowserTests::RunSample;
using NtfsBrowserTests::SampleRun;
namespace Fs = std::filesystem;

namespace {

// Stdout of both samples when the arguments are not one path and a --log.
constexpr std::string_view usage_text =
    R"(Invalid parameter
Usage: ntfsdir [--log=...] "path"
  --log=<console|file>:<off|error|warn|info|debug|trace>[:<path>]
eg. ntfsdir c:
eg. ntfsdir c:\windows
eg. ntfsdir "c:\program files\common files"
)";

// Stderr of both samples when the volume cannot be opened: the library's line.
constexpr std::string_view volume_open_error = "Cannot open volume\n";

// A drive letter with no drive mapped to it. Skips the test when D: to Z:
// are all taken, so that no test ever opens a real volume.
char RequireFreeDriveLetter() {
  const DWORD mapped = GetLogicalDrives();
  for (DWORD index = 'Z' - 'A'; index >= 'D' - 'A'; --index) {
    if ((mapped & (DWORD{1} << index)) == 0) {
      return static_cast<char>('A' + index);
    }
  }
  SKIP("every drive letter from D: to Z: is mapped");
}

// A path on drive letter, in the form the samples take: "X:" then rest.
std::wstring OnDrive(char letter, std::wstring_view rest) {
  return std::wstring(1, static_cast<wchar_t>(letter)) + L":" +
         std::wstring(rest);
}

// Stdout of a sample that cannot open the volume of letter.
std::string MissingVolumeOutput(char letter) {
  return std::string("Cannot get NTFS BPB from boot sector of volume ") +
         letter + "\n";
}

// A fresh temp path with the given extension, not yet created.
Fs::path TempPath(std::string_view extension) {
  std::random_device random_device;
  return Fs::temp_directory_path() /
         ("ntfsdir-sample-" + std::to_string(random_device()) +
          std::string(extension));
}

// Runs one sample with args, after checking that it was built.
SampleRun RunDir(const Fs::path& exe, const std::vector<std::wstring>& args) {
  REQUIRE(Fs::exists(exe));
  return RunSample(exe, args);
}

}  // namespace

TEST_CASE("NtfsDir and NtfsDir2 print usage without arguments",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {});

  CHECK(run.exit_code == -1);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir and NtfsDir2 print usage for two paths",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {L"c:\\", L"d:\\"});

  CHECK(run.exit_code == -1);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir and NtfsDir2 print usage for a malformed --log option",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {L"--log=bogus", L"c:\\"});

  CHECK(run.exit_code == -1);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir and NtfsDir2 print usage for a path without a drive letter",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {L"windows"});

  CHECK(run.exit_code == -1);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir and NtfsDir2 print usage for an empty path",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {L""});

  CHECK(run.exit_code == -1);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir and NtfsDir2 report a drive letter with no volume",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const SampleRun run = RunDir(exe, {OnDrive(letter, L"\\Docs\\sub")});

  CHECK(run.exit_code == -1);
  CHECK(run.out == MissingVolumeOutput(letter));
  CHECK(run.err == volume_open_error);
}

TEST_CASE("NtfsDir and NtfsDir2 --log=console:off silences the library line",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const SampleRun run =
      RunDir(exe, {L"--log=console:off", OnDrive(letter, L"\\")});

  CHECK(run.exit_code == -1);
  CHECK(run.out == MissingVolumeOutput(letter));
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir and NtfsDir2 accept --log before or after the path",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const std::wstring path = OnDrive(letter, L"\\");
  const SampleRun before = RunDir(exe, {L"--log=console:info", path});
  const SampleRun after = RunDir(exe, {path, L"--log=console:info"});

  CHECK(before.exit_code == -1);
  CHECK(before.out == MissingVolumeOutput(letter));
  CHECK(before.err == volume_open_error);
  CHECK(after.exit_code == before.exit_code);
  CHECK(after.out == before.out);
  CHECK(after.err == before.err);
}

TEST_CASE("NtfsDir and NtfsDir2 --log=file keeps the library line in the file",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const Fs::path log_path = TempPath(".log");
  const SampleRun run = RunDir(
      exe, {L"--log=file:trace:" + log_path.wstring(), OnDrive(letter, L"\\")});

  CHECK(run.exit_code == -1);
  CHECK(run.out == MissingVolumeOutput(letter));
  CHECK(run.err == volume_open_error);
  REQUIRE(Fs::exists(log_path));
  std::ifstream input(log_path);
  const std::string log_text((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  CHECK_THAT(log_text, ContainsSubstring("[error] Cannot open volume"));
  std::error_code ignored;
  Fs::remove(log_path, ignored);
}

TEST_CASE("NtfsDir and NtfsDir2 report a log path they cannot open and go on",
          "[io][sample][characterization]") {
  const Fs::path exe =
      GENERATE(values<Fs::path>({NTFS_DIR_EXE, NTFS_DIR2_EXE}));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const Fs::path log_dir = Fs::temp_directory_path();
  const SampleRun run = RunDir(
      exe, {L"--log=file:trace:" + log_dir.wstring(), OnDrive(letter, L"\\")});

  CHECK(run.exit_code == -1);
  CHECK(run.out == MissingVolumeOutput(letter));
  CHECK(run.err == "Cannot open log file " + log_dir.string() + "\n" +
                       std::string(volume_open_error));
}
