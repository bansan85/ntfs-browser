#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fake-ntfs-image.h"
#include "sample-run.h"

#include <windows.h>

using Catch::Matchers::ContainsSubstring;
using NtfsBrowserTests::SampleRun;
namespace Fs = std::filesystem;

namespace {

// $STANDARD_INFORMATION last write time of every record of the fake image.
constexpr ULONGLONG image_write_time = 0x1112131415161718ULL;

// The listings' "{write}" as NtfsMftList prints it. The library converts
// the time to the machine's local zone, so the text depends on the host.
std::string LocalWriteTime() {
  const ULARGE_INTEGER utc{.QuadPart = image_write_time};
  const FILETIME utc_ft{.dwLowDateTime = utc.LowPart,
                        .dwHighDateTime = utc.HighPart};
  FILETIME local_ft{};
  SYSTEMTIME local{};
  REQUIRE(FileTimeToLocalFileTime(&utc_ft, &local_ft) != 0);
  REQUIRE(FileTimeToSystemTime(&local_ft, &local) != 0);

  char text[32]{};
  snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", local.wYear,
           local.wMonth, local.wDay, local.wHour, local.wMinute);
  return text;
}

// Returns listing with every "{write}" replaced by LocalWriteTime().
std::string WithLocalTimes(std::string_view listing) {
  const std::string time = LocalWriteTime();
  constexpr std::string_view placeholder = "{write}";

  std::string result;
  while (true) {
    const size_t at = listing.find(placeholder);
    if (at == std::string_view::npos) {
      break;
    }
    result.append(listing.substr(0, at)).append(time);
    listing.remove_prefix(at + placeholder.size());
  }
  return result.append(listing);
}

// Stdout of NtfsMftList on BuildFakeNtfsImageWithMftTree(): the live records
// only, with the tree's hard link and lost-parent path.
constexpr std::string_view live_listing =
    R"(    Record   Seq               Size Last write       Attrib Ln Path
         0     1              26624 1601-01-01 00:00 ------  1 \$MFT
         3     0                  0 1601-01-01 00:00 ------  0 <no name>
         5     5              <DIR> {write} ------  1 \
        16     1              <DIR> {write} ------  1 \Docs
        17     3                 37 {write} R-----  1 \Docs\report.txt
        18     1                  0 {write} ------  2 \link-a
                                                               = \Docs\link-b
        23     7              <DIR> {write} ------  1 \NewDir

Record slots: 26, in use: 7, deleted: 4, extensions: 1
Unreadable: 14, damaged: 0, not linked to the root: 1
)";

// Stdout of NtfsMftList --deleted on the same image: the freed records
// join the listing, and the one under a lost parent shows <lost #23>.
constexpr std::string_view deleted_listing =
    R"(    Record   Seq               Size Last write       Attrib Ln Path
         0     1              26624 1601-01-01 00:00 ------  1 \$MFT
         3     0                  0 1601-01-01 00:00 ------  0 <no name>
         5     5              <DIR> {write} ------  1 \
        16     1              <DIR> {write} ------  1 \Docs
        17     3                 37 {write} R-----  1 \Docs\report.txt
        18     1                  0 {write} ------  2 \link-a
                                                               = \Docs\link-b
        19     2 DEL              0 {write} ------  1 \Docs\old.tmp
        20     4 DEL          <DIR> {write} ------  1 \Docs\OldDir
        21     2 DEL              0 {write} ------  1 \Docs\OldDir\draft.doc
        22     2 DEL              0 {write} ------  1 <lost #23>\stale.txt
        23     7              <DIR> {write} ------  1 \NewDir

Record slots: 26, in use: 7, deleted: 4, extensions: 1
Unreadable: 14, damaged: 0, not linked to the root: 2
)";

// Stderr of NtfsMftList on a file that is not a volume: the library's own
// error line, then the sample's.
constexpr std::string_view missing_volume_error =
    "Cannnot open volume\n"
    "Cannot open no-such-volume.img as an NTFS volume\n";

// Runs NtfsMftList with args, its stdout and stderr captured apart.
SampleRun RunSample(const std::vector<std::wstring>& args) {
  const Fs::path exe(NTFS_MFT_LIST_EXE);
  REQUIRE(Fs::exists(exe));
  return NtfsBrowserTests::RunSample(exe, args);
}

// Writes BuildFakeNtfsImageWithMftTree() to a temp file, removed on exit.
struct TempTreeImage final {
  Fs::path path = NtfsBrowserTests::WriteFakeNtfsImage(
      NtfsBrowserTests::BuildFakeNtfsImageWithMftTree());

  TempTreeImage() = default;

  ~TempTreeImage() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  TempTreeImage(const TempTreeImage&) = delete;
  TempTreeImage& operator=(const TempTreeImage&) = delete;
  TempTreeImage(TempTreeImage&&) = delete;
  TempTreeImage& operator=(TempTreeImage&&) = delete;
};

}  // namespace

TEST_CASE("NtfsMftList lists the live records of the MFT tree image",
          "[mft][sample][characterization]") {
  const TempTreeImage image;
  const SampleRun run = RunSample({image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == WithLocalTimes(live_listing));
}

TEST_CASE("NtfsMftList --deleted also lists the freed records",
          "[mft][sample][characterization]") {
  const TempTreeImage image;
  const SampleRun run = RunSample({L"--deleted", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == WithLocalTimes(deleted_listing));
}

TEST_CASE("NtfsMftList --recover on a clean image lists what it lists without",
          "[mft][sample][characterization]") {
  const TempTreeImage image;
  const SampleRun plain = RunSample({L"--deleted", image.path.wstring()});
  const SampleRun recovering =
      RunSample({L"--deleted", L"--recover", image.path.wstring()});

  CHECK(recovering.exit_code == 0);
  CHECK(recovering.out == plain.out);
}

TEST_CASE("NtfsMftList without a target prints usage and fails",
          "[mft][sample][characterization]") {
  const SampleRun run = RunSample({L"--deleted"});

  CHECK(run.exit_code == -1);
  CHECK_THAT(run.out, ContainsSubstring("Usage: ntfsmftlist"));
}

TEST_CASE("NtfsMftList with two targets prints usage and fails",
          "[mft][sample][characterization]") {
  const SampleRun run = RunSample({L"first.img", L"second.img"});

  CHECK(run.exit_code == -1);
  CHECK_THAT(run.out, ContainsSubstring("Usage: ntfsmftlist"));
}

TEST_CASE("NtfsMftList rejects a malformed --log option with usage",
          "[mft][sample][characterization]") {
  const TempTreeImage image;
  const SampleRun run = RunSample({L"--log=bogus", image.path.wstring()});

  CHECK(run.exit_code == -1);
  CHECK_THAT(run.out, ContainsSubstring("Usage: ntfsmftlist"));
}

TEST_CASE("NtfsMftList reports a target that is not an NTFS volume",
          "[mft][sample][characterization]") {
  const SampleRun run = RunSample({L"no-such-volume.img"});

  CHECK(run.exit_code == -1);
  CHECK(run.out.empty());
  CHECK(run.err == missing_volume_error);
}
