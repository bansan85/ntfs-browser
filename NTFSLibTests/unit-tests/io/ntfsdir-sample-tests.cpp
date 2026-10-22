#include <ntfs-browser/win-types.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>

#ifndef _WIN32
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "local-time.h"
#include "sample-run.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Tests::dump_broken_alter_time;
using NTFS_BROWSER_NAMESPACE::Tests::RunSample;
using NTFS_BROWSER_NAMESPACE::Tests::sample_failure_exit_code;
using NTFS_BROWSER_NAMESPACE::Tests::SampleRun;
namespace Fs = std::filesystem;

namespace {

#ifdef _WIN32

// Stdout of both samples when the arguments are not one path and a --log.
constexpr std::string_view usage_text =
    R"(Invalid parameter
Usage: ntfsdir [--log=...] "path"
  --log=<console|file>:<off|error|warn|info|debug|trace>[:<path>]
eg. ntfsdir c:
eg. ntfsdir c:\windows
eg. ntfsdir "c:\program files\common files"
eg. ntfsdir c:\image.img\Windows
)";

#else

// Stdout of the sample when the arguments are not one path and a --log.
constexpr std::string_view usage_text =
    R"(Invalid parameter
Usage: ntfsdir [--log=...] "path"
  --log=<console|file>:<off|error|warn|info|debug|trace>[:<path>]
eg. ntfsdir /dev/sda1
eg. ntfsdir /dev/sda1/Windows
eg. ntfsdir "/dev/sda1/Program Files/Common Files"
eg. ntfsdir ./disk.img/Windows
)";

#endif

// The samples under test: NtfsDir on every platform, and NtfsDir2, the same
// listing over the other cache strategy, where it is built.
std::vector<Fs::path> SampleExes() {
#ifdef _WIN32
  return {NTFS_DIR_EXE, NTFS_DIR2_EXE};
#else
  return {NTFS_DIR_EXE};
#endif
}

// A path argument that opens nothing, for the tests that stop at parsing.
#ifdef _WIN32
constexpr std::wstring_view any_target = L"c:\\";
#else
constexpr std::wstring_view any_target = L"/dev/sda1";
#endif

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

// Writes an NTFS image to a temp file, removed on exit. The default holds
// "Docs" in the root and "report.txt" in Docs, with a known write time.
struct TempImage final {
  Fs::path path;

  TempImage() : TempImage(L"report.txt") {}

  explicit TempImage(std::wstring_view report_name)
      : TempImage(
            NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithReportName(
                report_name, dump_broken_alter_time)) {}

  explicit TempImage(const std::vector<BYTE>& image)
      : path(NTFS_BROWSER_NAMESPACE::Tests::WriteFakeNtfsImage(image)) {}

  ~TempImage() {
    std::error_code ignored;
    Fs::remove(path, ignored);
  }

  TempImage(const TempImage&) = delete;
  TempImage& operator=(const TempImage&) = delete;
  TempImage(TempImage&&) = delete;
  TempImage& operator=(TempImage&&) = delete;
};

// The listing line of an entry of size zero and write time ticks (UTC), as
// NtfsDir prints it in the host's time zone. Empty where the platform cannot
// show that time, which leaves the line out.
std::string ListingLine(std::string_view name, bool directory,
                        ULONGLONG ticks) {
  const auto local = NtfsDevice::UtcTicksToLocalTime(ticks);
  if (!local) {
    return {};
  }

  char text[160]{};
  snprintf(text, sizeof(text), "%u-%02u-%02u  %02u:%02u\t%s    %s<   >\t",
           local->year, local->month, local->day, local->hour, local->minute,
           directory ? "<DIR>" : "     ", directory ? "\t" : "0\t");
  return std::string(text) + std::string(name) + "\n";
}

// The listing of the root of a TempImage: only "Docs", whose index entry has
// no write time.
std::string RootListing() {
  return ListingLine("Docs", true, 0) + "Files: 0, Directories: 1\n";
}

// The listing of Docs in a TempImage: one file called name.
std::string DocsListing(std::string_view name) {
  return ListingLine(name, false, dump_broken_alter_time) +
         "Files: 1, Directories: 0\n";
}

// The path of image with directory after it, in the form the samples take.
std::wstring InImage(const Fs::path& image, std::wstring_view directory) {
  return image.wstring() + L"/" + std::wstring(directory);
}

}  // namespace

TEST_CASE("NtfsDir prints usage without arguments",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir prints usage for two paths",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {std::wstring(any_target), L"/other"});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir prints usage for a malformed --log option",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {L"--log=bogus", std::wstring(any_target)});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir does not take --deleted or --recover",
          "[io][sample][characterization]") {
  const TempImage image;

  for (const std::wstring option : {L"--deleted", L"--recover"}) {
    INFO(Fs::path(option).string());
    const SampleRun run = RunDir(NTFS_DIR_EXE, {option, image.path.wstring()});

    CHECK(run.exit_code == sample_failure_exit_code);
    CHECK(run.out == usage_text);
  }
}

TEST_CASE("NtfsDir usage names the form of the platform",
          "[io][sample][characterization]") {
  const SampleRun run = RunDir(NTFS_DIR_EXE, {});

#ifdef _WIN32
  CHECK_THAT(run.out, ContainsSubstring("eg. ntfsdir c:\n"));
#else
  CHECK_THAT(run.out, ContainsSubstring("eg. ntfsdir /dev/sda1/Windows\n"));
  CHECK_THAT(run.out, ContainsSubstring("eg. ntfsdir ./disk.img/Windows\n"));
#endif
}

TEST_CASE("NtfsDir lists the root of an image file",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const TempImage image;
  const SampleRun run = RunDir(exe, {image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == RootListing());
}

TEST_CASE("NtfsDir lists a directory of an image file",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const TempImage image;
  const SampleRun run = RunDir(exe, {InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("report.txt"));
}

TEST_CASE("NtfsDir finds a directory whatever its case",
          "[io][sample][characterization]") {
  const TempImage image;
  const SampleRun exact = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"Docs")});
  const SampleRun lower = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"docs")});
  const SampleRun slash = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"DOCS/")});

  CHECK(exact.out == DocsListing("report.txt"));
  CHECK(lower.exit_code == 0);
  CHECK(lower.out == exact.out);
  CHECK(slash.exit_code == 0);
  CHECK(slash.out == exact.out);
}

TEST_CASE("NtfsDir accepts --log before or after the image",
          "[io][sample][characterization]") {
  const TempImage image;
  const SampleRun before =
      RunDir(NTFS_DIR_EXE, {L"--log=console:off", image.path.wstring()});
  const SampleRun after =
      RunDir(NTFS_DIR_EXE, {image.path.wstring(), L"--log=console:off"});

  CHECK(before.exit_code == 0);
  CHECK(before.out == RootListing());
  CHECK(after.exit_code == before.exit_code);
  CHECK(after.out == before.out);
}

TEST_CASE("NtfsDir reports a directory that does not exist",
          "[io][sample][characterization]") {
  const TempImage image;
  const SampleRun run = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"Nope")});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == "Cannot find directory Nope\n");
}

TEST_CASE("NtfsDir reports a file where a directory is wanted",
          "[io][sample][characterization]") {
  const TempImage image;
  const SampleRun run =
      RunDir(NTFS_DIR_EXE, {InImage(image.path, L"Docs/report.txt")});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == "report.txt is not a directory\n");
}

TEST_CASE("NtfsDir reports an image that is not an NTFS volume",
          "[io][sample][characterization]") {
  const TempImage image(std::vector<BYTE>(4096, 0));
  const SampleRun run =
      RunDir(NTFS_DIR_EXE, {L"--log=console:off", image.path.wstring()});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == "Cannot get NTFS BPB from boot sector of " +
                       image.path.string() + "\n");
}

#ifdef _WIN32

namespace {

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

// Stderr of both samples when the volume cannot be opened: the library's line.
constexpr std::string_view volume_open_error = "Cannot open volume\n";

}  // namespace

TEST_CASE("NtfsDir and NtfsDir2 print usage for a path without a drive letter",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {L"windows"});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir and NtfsDir2 print usage for an empty path",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const SampleRun run = RunDir(exe, {L""});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == usage_text);
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir and NtfsDir2 report a drive letter with no volume",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const SampleRun run = RunDir(exe, {OnDrive(letter, L"\\Docs\\sub")});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == MissingVolumeOutput(letter));
  CHECK(run.err == volume_open_error);
}

TEST_CASE("NtfsDir and NtfsDir2 --log=console:off silences the library line",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const SampleRun run =
      RunDir(exe, {L"--log=console:off", OnDrive(letter, L"\\")});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == MissingVolumeOutput(letter));
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir and NtfsDir2 accept --log before or after the path",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const std::wstring path = OnDrive(letter, L"\\");
  const SampleRun before = RunDir(exe, {L"--log=console:info", path});
  const SampleRun after = RunDir(exe, {path, L"--log=console:info"});

  CHECK(before.exit_code == sample_failure_exit_code);
  CHECK(before.out == MissingVolumeOutput(letter));
  CHECK(before.err == volume_open_error);
  CHECK(after.exit_code == before.exit_code);
  CHECK(after.out == before.out);
  CHECK(after.err == before.err);
}

TEST_CASE("NtfsDir and NtfsDir2 --log=file keeps the library line in the file",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const Fs::path log_path = TempPath(".log");
  const SampleRun run = RunDir(
      exe, {L"--log=file:trace:" + log_path.wstring(), OnDrive(letter, L"\\")});

  CHECK(run.exit_code == sample_failure_exit_code);
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
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const char letter = RequireFreeDriveLetter();
  const Fs::path log_dir = Fs::temp_directory_path();
  const SampleRun run = RunDir(
      exe, {L"--log=file:trace:" + log_dir.wstring(), OnDrive(letter, L"\\")});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == MissingVolumeOutput(letter));
  CHECK(run.err == "Cannot open log file " + log_dir.string() + "\n" +
                       std::string(volume_open_error));
}

#else

TEST_CASE("NtfsDir reports a path that holds no device or image",
          "[io][sample][characterization]") {
  const SampleRun missing = RunDir(NTFS_DIR_EXE, {L"/no/such/device"});
  const SampleRun empty = RunDir(NTFS_DIR_EXE, {L""});
  const SampleRun relative = RunDir(NTFS_DIR_EXE, {L"windows"});

  CHECK(missing.exit_code == sample_failure_exit_code);
  CHECK(missing.out ==
        "Cannot find a device or image file in /no/such/device\n");
  CHECK(empty.exit_code == sample_failure_exit_code);
  CHECK(empty.out == "Cannot find a device or image file in \n");
  CHECK(relative.exit_code == sample_failure_exit_code);
  CHECK(relative.out == "Cannot find a device or image file in windows\n");
}

TEST_CASE("NtfsDir reports a device it may not read",
          "[io][sample][characterization]") {
  if (geteuid() == 0) {
    SKIP("root can read any file");
  }
  const TempImage image;
  REQUIRE(chmod(image.path.c_str(), 0) == 0);
  const SampleRun run = RunDir(NTFS_DIR_EXE, {image.path.wstring()});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out == "Cannot read device " + image.path.string() +
                       ": read access is needed\n");
}

TEST_CASE("NtfsDir goes on after a log path it cannot open",
          "[io][sample][characterization]") {
  const TempImage image;
  const Fs::path log_dir = Fs::temp_directory_path();
  const SampleRun run =
      RunDir(NTFS_DIR_EXE,
             {L"--log=file:trace:" + log_dir.wstring(), image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == RootListing());
  CHECK_THAT(run.err, ContainsSubstring("Cannot open log file " +
                                        log_dir.string() + "\n"));
}

TEST_CASE("NtfsDir prints an accented name as UTF-8",
          "[io][sample][characterization]") {
  const TempImage image(L"caf\u00E9.txt");
  const SampleRun run = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("caf\xC3\xA9.txt"));
}

TEST_CASE("NtfsDir prints an unpaired surrogate in its WTF-8 form",
          "[io][sample][characterization]") {
  const TempImage image(std::wstring{static_cast<wchar_t>(0xD800), L'a'});
  const SampleRun run = RunDir(NTFS_DIR_EXE, {InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("\xED\xA0\x80"
                               "a"));
}

#endif
