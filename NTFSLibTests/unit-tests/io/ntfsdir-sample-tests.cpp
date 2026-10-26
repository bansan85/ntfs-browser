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
#include "ntfsdir-sample-support.h"
#include "sample-run.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Tests::dump_broken_alter_time;
using NTFS_BROWSER_NAMESPACE::Tests::sample_failure_exit_code;
using NTFS_BROWSER_NAMESPACE::Tests::SampleRun;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::Below;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::DirectoryHeader;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::DocsListing;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::InImage;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::ListingLine;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::RootListing;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::RunDir;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::SampleExes;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::SkipLine;
using NTFS_BROWSER_NAMESPACE::Tests::DirSample::TempImage;
namespace Fs = std::filesystem;

namespace {

#ifdef _WIN32

// Stdout of both samples when the arguments are not one path and a --log.
constexpr std::string_view usage_text =
    R"(Invalid parameter
Usage: ntfsdir [--recursive] [--deleted] [--recover] [--log=...] "path"
  --recursive, -r  also list every subdirectory of path
  --deleted        also list deleted files, marked with *
  --recover        salvage damaged directory data (? marks a name whose file record cannot be read)
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
Usage: ntfsdir [--recursive] [--deleted] [--recover] [--log=...] "path"
  --recursive, -r  also list every subdirectory of path
  --deleted        also list deleted files, marked with *
  --recover        salvage damaged directory data (? marks a name whose file record cannot be read)
  --log=<console|file>:<off|error|warn|info|debug|trace>[:<path>]
eg. ntfsdir /dev/sda1
eg. ntfsdir /dev/sda1/Windows
eg. ntfsdir "/dev/sda1/Program Files/Common Files"
eg. ntfsdir ./disk.img/Windows
)";

#endif

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

TEST_CASE("NtfsDir takes --recover without changing a healthy listing",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const TempImage image;
  const SampleRun run =
      RunDir(exe, {L"--recover", InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("report.txt"));
}

TEST_CASE("NtfsDir --deleted adds a marker column to a listing",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const TempImage image;
  const SampleRun run =
      RunDir(exe, {InImage(image.path, L"Docs"), L"--deleted"});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("report.txt", " "));
}

TEST_CASE("NtfsDir --recursive lists every directory under a header",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const TempImage image;
  const std::string root = image.path.string();
  const std::string docs =
      root + static_cast<char>(Fs::path::preferred_separator) + "Docs";

  for (const std::wstring option : {L"--recursive", L"-r"}) {
    INFO(Fs::path(option).string());
    const SampleRun run = RunDir(exe, {option, image.path.wstring()});

    CHECK(run.exit_code == 0);
    CHECK(run.out ==
          "\nDirectory of " + root + "\n" + ListingLine("Docs", true, 0) +
              "\nDirectory of " + docs + "\n" +
              ListingLine("report.txt", false, dump_broken_alter_time) +
              "Files: 1, Directories: 1\n");
  }
}

TEST_CASE("NtfsDir --recursive enters a directory once when an entry loops",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const TempImage image(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithDirectoryLoop());
  const std::string root = image.path.string();
  const std::string docs = Below(root, "Docs");
  const std::string sub = Below(docs, "Sub");
  const std::string back = Below(sub, "Back");
  const SampleRun run = RunDir(exe, {L"--recursive", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DirectoryHeader(root) + ListingLine("Docs", true, 0) +
                       DirectoryHeader(docs) + ListingLine("Sub", true, 0) +
                       DirectoryHeader(sub) + ListingLine("Back", true, 0) +
                       SkipLine(back) + "Files: 0, Directories: 3\n");
}

TEST_CASE(
    "NtfsDir --recursive --recover skips a stale orphaned entry that loops",
    "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const TempImage image(NTFS_BROWSER_NAMESPACE::Tests::
                            BuildFakeNtfsImageWithOrphanedDirectoryLoop());
  const std::string root = image.path.string();
  const std::string docs = Below(root, "Docs");
  const std::string sub = Below(docs, "Sub");
  const std::string again = Below(sub, "Docs");
  const SampleRun run =
      RunDir(exe, {L"--recursive", L"--recover", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DirectoryHeader(root) + ListingLine("Docs", true, 0) +
                       DirectoryHeader(docs) + ListingLine("Sub", true, 0) +
                       DirectoryHeader(sub) + ListingLine("Docs", true, 0) +
                       SkipLine(again) + "Files: 0, Directories: 3\n");
}

TEST_CASE("NtfsDir --recursive does not show a stale entry without --recover",
          "[io][sample][characterization]") {
  const Fs::path exe = GENERATE_COPY(from_range(SampleExes()));
  INFO(exe.string());
  const TempImage image(NTFS_BROWSER_NAMESPACE::Tests::
                            BuildFakeNtfsImageWithOrphanedDirectoryLoop());
  const std::string root = image.path.string();
  const std::string docs = Below(root, "Docs");
  const std::string sub = Below(docs, "Sub");
  const SampleRun run = RunDir(exe, {L"--recursive", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DirectoryHeader(root) + ListingLine("Docs", true, 0) +
                       DirectoryHeader(docs) + ListingLine("Sub", true, 0) +
                       DirectoryHeader(sub) + "Files: 0, Directories: 2\n");
}

TEST_CASE("NtfsDir --recursive starts at the directory it is given",
          "[io][sample][characterization]") {
  const TempImage image;
  const SampleRun run =
      RunDir(NTFS_DIR_EXE, {L"-r", InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring("\nDirectory of "));
  CHECK_THAT(run.out, ContainsSubstring("report.txt\n"));
  CHECK_THAT(run.out, ContainsSubstring("Files: 1, Directories: 0\n"));
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

#endif
