#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "child-process.h"
#include "fake-ntfs-image.h"

using Catch::Matchers::ContainsSubstring;
using NtfsBrowserTests::RunProcessToFiles;
namespace Fs = std::filesystem;

namespace {

// Stdout of NtfsMftList on BuildFakeNtfsImageWithMftTree(): the live records
// only, with the tree's hard link and lost-parent path.
constexpr std::string_view live_listing =
    R"(    Record   Seq               Size Last write       Attrib Ln Path
         0     1              26624 1601-01-01 00:00 ------  1 \$MFT
         3     0                  0 1601-01-01 00:00 ------  0 <no name>
         5     5              <DIR> 5498-12-05 07:21 ------  1 \
        16     1              <DIR> 5498-12-05 07:21 ------  1 \Docs
        17     3                 37 5498-12-05 07:21 R-----  1 \Docs\report.txt
        18     1                  0 5498-12-05 07:21 ------  2 \link-a
                                                               = \Docs\link-b
        23     7              <DIR> 5498-12-05 07:21 ------  1 \NewDir

Record slots: 26, in use: 7, deleted: 4, extensions: 1
Unreadable: 14, damaged: 0, not linked to the root: 1
)";

// Stdout of NtfsMftList --deleted on the same image: the freed records
// join the listing, and the one under a lost parent shows <lost #23>.
constexpr std::string_view deleted_listing =
    R"(    Record   Seq               Size Last write       Attrib Ln Path
         0     1              26624 1601-01-01 00:00 ------  1 \$MFT
         3     0                  0 1601-01-01 00:00 ------  0 <no name>
         5     5              <DIR> 5498-12-05 07:21 ------  1 \
        16     1              <DIR> 5498-12-05 07:21 ------  1 \Docs
        17     3                 37 5498-12-05 07:21 R-----  1 \Docs\report.txt
        18     1                  0 5498-12-05 07:21 ------  2 \link-a
                                                               = \Docs\link-b
        19     2 DEL              0 5498-12-05 07:21 ------  1 \Docs\old.tmp
        20     4 DEL          <DIR> 5498-12-05 07:21 ------  1 \Docs\OldDir
        21     2 DEL              0 5498-12-05 07:21 ------  1 \Docs\OldDir\draft.doc
        22     2 DEL              0 5498-12-05 07:21 ------  1 <lost #23>\stale.txt
        23     7              <DIR> 5498-12-05 07:21 ------  1 \NewDir

Record slots: 26, in use: 7, deleted: 4, extensions: 1
Unreadable: 14, damaged: 0, not linked to the root: 2
)";

// Stderr of NtfsMftList on a file that is not a volume: the library's own
// error line, then the sample's.
constexpr std::string_view missing_volume_error =
    "Cannnot open volume\n"
    "Cannot open no-such-volume.img as an NTFS volume\n";

// Exit code and text of one NtfsMftList run. The CRT writes CRLF on Windows,
// so the text has its carriage returns removed.
struct SampleRun {
  int exit_code = 0;
  std::string out;
  std::string err;
};

// Reads a whole file as text, without its carriage returns.
std::string ReadText(const Fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  std::erase(text, '\r');
  return text;
}

// A fresh temp path for one of the run's redirected streams.
Fs::path TempStreamPath(std::string_view tag) {
  std::random_device random_device;
  return Fs::temp_directory_path() /
         ("ntfsmftlist-" + std::to_string(random_device()) + "-" +
          std::string(tag) + ".txt");
}

// Runs NtfsMftList with args, its stdout and stderr captured apart.
SampleRun RunSample(const std::vector<std::wstring>& args) {
  const Fs::path exe(NTFS_MFT_LIST_EXE);
  REQUIRE(Fs::exists(exe));

  const Fs::path out_path = TempStreamPath("out");
  const Fs::path err_path = TempStreamPath("err");
  SampleRun run;
  run.exit_code = RunProcessToFiles(exe, args, out_path, err_path);
  run.out = ReadText(out_path);
  run.err = ReadText(err_path);
  std::filesystem::remove(out_path);
  std::filesystem::remove(err_path);
  return run;
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
  CHECK(run.out == live_listing);
}

TEST_CASE("NtfsMftList --deleted also lists the freed records",
          "[mft][sample][characterization]") {
  const TempTreeImage image;
  const SampleRun run = RunSample({L"--deleted", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == deleted_listing);
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
