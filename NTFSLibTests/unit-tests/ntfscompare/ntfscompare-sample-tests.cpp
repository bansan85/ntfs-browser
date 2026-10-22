#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "sample-run.h"
#include "temp-dir.h"

#ifndef _WIN32
  #include <sys/stat.h>
  #include <unistd.h>

  #include "fake-ntfs-image.h"
#endif

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Tests::RunSample;
using NTFS_BROWSER_NAMESPACE::Tests::SampleRun;
namespace Fs = std::filesystem;

namespace {

#ifndef _WIN32

// An NTFS image in a temp file, removed on exit. Docs holds one file.
struct TempImage final {
  Fs::path path;

  TempImage()
      : path(NTFS_BROWSER_NAMESPACE::Tests::WriteFakeNtfsImage(
            NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithReportName(
                L"report.txt"))) {}

  ~TempImage() {
    std::error_code ignored;
    Fs::remove(path, ignored);
  }

  TempImage(const TempImage&) = delete;
  TempImage& operator=(const TempImage&) = delete;
  TempImage(TempImage&&) = delete;
  TempImage& operator=(TempImage&&) = delete;
};

using NTFS_BROWSER_NAMESPACE::Tests::TempDir;

#endif

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
  CHECK_THAT(run.err, ContainsSubstring("<folder"));
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

TEST_CASE("NtfsCompare accepts --summary before or after the folder",
          "[ntfscompare][sample][characterization]") {
  const SampleRun before =
      RunCompare({L"--summary", L"ntfscompare-no-such-folder"});
  const SampleRun after =
      RunCompare({L"ntfscompare-no-such-folder", L"--summary"});

  CHECK(before.exit_code == 1);
  CHECK(before.err == "ntfscompare-no-such-folder is not a directory\n");
  CHECK(after.exit_code == 1);
  CHECK(after.err == before.err);
}

TEST_CASE("NtfsCompare prints usage for --summary alone",
          "[ntfscompare][sample][characterization]") {
  const SampleRun run = RunCompare({L"--summary"});

  CHECK(run.exit_code == 1);
  CHECK_THAT(run.err, ContainsSubstring("[--summary]"));
  CHECK(run.out.empty());
}

TEST_CASE("NtfsCompare refuses a missing folder before opening any volume",
          "[ntfscompare][sample][characterization]") {
  const SampleRun run = RunCompare({L"ntfscompare-no-such-folder"});

  CHECK(run.exit_code == 1);
  CHECK(run.err == "ntfscompare-no-such-folder is not a directory\n");
  CHECK(run.out.empty());
}

#ifdef _WIN32

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

#else

TEST_CASE("NtfsCompare reads a file that is not NTFS as a device",
          "[ntfscompare][sample][characterization]") {
  const TempDir dir;
  const Fs::path file = dir.Write("not-ntfs.img", std::string(4096, '\0'));

  const SampleRun run = RunCompare({file.wstring()});

  CHECK(run.exit_code == 1);
  CHECK_THAT(run.err,
             ContainsSubstring(file.string() + " is not an NTFS volume\n"));
  CHECK(run.out.empty());
}

TEST_CASE("NtfsCompare skips the operating system for an unmounted device",
          "[ntfscompare][sample][characterization]") {
  const TempImage image;

  const SampleRun run = RunCompare({image.path.wstring()});

  // The fake image's index entries and records disagree, which is no
  // mismatch: the status only depends on the names and the cache strategies.
  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring("Not mounted: the operating-system "
                                        "comparison is skipped.\n"));
  CHECK_THAT(run.out, !ContainsSubstring("[LIB-MISMATCH]"));
  CHECK_THAT(run.err, ContainsSubstring("via NtfsVolume<FullCache>"));
  CHECK_THAT(run.err, ContainsSubstring("via NtfsVolume<NoCache>"));
  CHECK_THAT(run.err, !ContainsSubstring("via std::filesystem"));
  CHECK_THAT(run.err, !ContainsSubstring("via Linux API"));
}

TEST_CASE("NtfsCompare takes a directory after the device",
          "[ntfscompare][sample][characterization]") {
  const TempImage image;

  const SampleRun run = RunCompare({image.path.wstring() + L"/Docs"});

  CHECK_THAT(run.out, ContainsSubstring("Not mounted:"));
  CHECK_THAT(run.err, ContainsSubstring("Docs via NtfsVolume<FullCache>"));
}

TEST_CASE("NtfsCompare reports a directory the device does not hold",
          "[ntfscompare][sample][characterization]") {
  const TempImage image;

  const SampleRun run = RunCompare({image.path.wstring() + L"/Nope"});

  CHECK(run.exit_code == 1);
  CHECK_THAT(run.err, ContainsSubstring("Cannot resolve"));
}

TEST_CASE("NtfsCompare reports a device it may not read",
          "[ntfscompare][sample][characterization]") {
  if (geteuid() == 0) {
    SKIP("root can read any file");
  }
  const TempImage image;
  REQUIRE(chmod(image.path.c_str(), 0) == 0);

  const SampleRun run = RunCompare({image.path.wstring()});
  INFO(run.out);
  INFO(run.err);

  CHECK(run.exit_code == 1);
  CHECK_THAT(run.err,
             ContainsSubstring("Cannot read device " + image.path.string() +
                               ": read access is needed\n"));
}

#endif
