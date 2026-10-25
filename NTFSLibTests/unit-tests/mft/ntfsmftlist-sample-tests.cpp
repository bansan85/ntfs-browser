#include <ntfs-browser/win-types.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "data/std-info-permission.h"
#include "fake-ntfs-image.h"
#include "local-time.h"
#include "mft-tree-parent-support.h"
#include "sample-run.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Tests::sample_failure_exit_code;
using NTFS_BROWSER_NAMESPACE::Tests::SampleRun;
namespace Fs = std::filesystem;

namespace {

// $STANDARD_INFORMATION last write time of every record of the fake image.
constexpr ULONGLONG image_write_time = 0x1112131415161718ULL;

// The listings' "{write}" as NtfsMftList prints it. The sample converts
// the time to the machine's local zone, so the text depends on the host.
std::string LocalWriteTime() {
  const auto local = NtfsDevice::UtcTicksToLocalTime(image_write_time);
  REQUIRE(local.has_value());

  char text[32]{};
  snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", local->year,
           local->month, local->day, local->hour, local->minute);
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

Record slots: 26, in use: 7, deleted: 4, extensions: 1 (0 reused by others)
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

Record slots: 26, in use: 7, deleted: 4, extensions: 1 (0 reused by others)
Unreadable: 14, damaged: 0, not linked to the root: 2
)";

// Stderr of NtfsMftList on a target that does not exist. Windows hands the
// path to the library, which logs its own error line first. Off Windows the
// sample finds no device or image to give the library.
#ifdef _WIN32
constexpr std::string_view missing_volume_error =
    "Cannot open volume\n"
    "Cannot open no-such-volume.img as an NTFS volume\n";
#else
constexpr std::string_view missing_volume_error =
    "Cannot open no-such-volume.img as an NTFS volume\n";
#endif

// Runs NtfsMftList with args, its stdout and stderr captured apart.
SampleRun RunSample(const std::vector<std::wstring>& args) {
  const Fs::path exe(NTFS_MFT_LIST_EXE);
  REQUIRE(Fs::exists(exe));
  return NTFS_BROWSER_NAMESPACE::Tests::RunSample(exe, args);
}

// Writes an MFT tree image to a temp file, removed on exit. The default is
// BuildFakeNtfsImageWithMftTree().
struct TempTreeImage final {
  Fs::path path;

  TempTreeImage()
      : TempTreeImage(
            NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree()) {}

  explicit TempTreeImage(const std::vector<BYTE>& image)
      : path(NTFS_BROWSER_NAMESPACE::Tests::WriteFakeNtfsImage(image)) {}

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

TEST_CASE("NtfsMftList shows where the chain breaks in the paths under a loop",
          "[mft][sample][characterization]") {
  std::vector<BYTE> tree =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  NTFS_BROWSER_NAMESPACE::Tests::MftTreeParents::LoopDocsAndNewDir(tree);
  const TempTreeImage image(tree);
  const SampleRun run = RunSample({image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring("  1 <lost #16>\\NewDir\\Docs\n"));
  CHECK_THAT(run.out,
             ContainsSubstring("  1 <lost #16>\\NewDir\\Docs\\report.txt\n"));
  CHECK_THAT(run.out, ContainsSubstring("  2 \\link-a\n"));
  CHECK_THAT(run.out,
             ContainsSubstring("= <lost #16>\\NewDir\\Docs\\link-b\n"));
  CHECK_THAT(run.out, ContainsSubstring("  1 <lost #23>\\Docs\\NewDir\n"));
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

namespace {

// The journal's own time, as the sample prints it.
std::string JournalTimeText() {
  const auto local = NtfsDevice::UtcTicksToLocalTime(
      NTFS_BROWSER_NAMESPACE::Tests::fake_usn_default_time);
  REQUIRE(local.has_value());

  char text[32]{};
  snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u", local->year,
           local->month, local->day, local->hour, local->minute);
  return text;
}

// The tree image with a journal that names the live report.txt, the freed
// old.tmp, and a file no record names, in Docs.
std::vector<BYTE> ImageWithJournal() {
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithFreedFileTree();
  NTFS_BROWSER_NAMESPACE::Tests::PutUsnJournal(
      image,
      {.records = {
           {.name = L"report.txt",
            .file_record = NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx,
            .file_sequence = 3,
            .parent_record = NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx,
            .parent_sequence = 1,
            .reason = 0x00000100},
           {.name = L"old.tmp",
            .file_record =
                NTFS_BROWSER_NAMESPACE::Tests::mft_tree_deleted_file_idx,
            .file_sequence = 1,
            .parent_record = NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx,
            .parent_sequence = 1,
            .reason = 0x80000200},
           {.name = L"ghost.bin",
            .file_record = 40,
            .file_sequence = 2,
            .parent_record = NTFS_BROWSER_NAMESPACE::Tests::mft_tree_docs_idx,
            .parent_sequence = 1,
            .reason = 0x80000200}}});
  return image;
}

}  // namespace

TEST_CASE("NtfsMftList --recover --deleted shows what the journal knows",
          "[mft][sample][usn]") {
  const TempTreeImage image(ImageWithJournal());
  const SampleRun run =
      RunSample({L"--deleted", L"--recover", image.path.wstring()});

  CHECK(run.exit_code == 0);
  const std::string time = JournalTimeText();
  CHECK_THAT(run.out, ContainsSubstring("\\Docs\\report.txt\n"));
  CHECK_THAT(run.out,
             ContainsSubstring("~ " + time + " FileCreate (1 records)\n"));
  CHECK_THAT(run.out,
             ContainsSubstring("~ " + time + " FileDelete|Close, deleted " +
                               time + " (1 records)\n"));
  CHECK_THAT(run.out, ContainsSubstring("Files only the journal names: 1\n"));
  CHECK_THAT(run.out,
             ContainsSubstring("\\Docs\\ghost.bin [FileDelete|Close]\n"));
  CHECK_THAT(run.out,
             ContainsSubstring("Journal: 3 records, 2 files annotated, 1 only "
                               "in the journal, 0 damaged\n"));
}

TEST_CASE("NtfsMftList --recover without --deleted lists no journal-only file",
          "[mft][sample][usn]") {
  const TempTreeImage image(ImageWithJournal());
  const SampleRun run = RunSample({L"--recover", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring("~ " + JournalTimeText() +
                                        " FileCreate (1 records)\n"));
  CHECK_THAT(run.out, !ContainsSubstring("Files only the journal names"));
}

TEST_CASE("NtfsMftList without --recover ignores the journal",
          "[mft][sample][usn]") {
  const TempTreeImage image(ImageWithJournal());
  const SampleRun run = RunSample({L"--deleted", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, !ContainsSubstring("~ "));
  CHECK_THAT(run.out, !ContainsSubstring("Files only the journal names"));
  CHECK_THAT(run.out, !ContainsSubstring("Journal:"));
}

TEST_CASE("NtfsMftList without a target prints usage and fails",
          "[mft][sample][characterization]") {
  const SampleRun run = RunSample({L"--deleted"});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK_THAT(run.out, ContainsSubstring("Usage: ntfsmftlist"));
}

TEST_CASE("NtfsMftList with two targets prints usage and fails",
          "[mft][sample][characterization]") {
  const SampleRun run = RunSample({L"first.img", L"second.img"});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK_THAT(run.out, ContainsSubstring("Usage: ntfsmftlist"));
}

TEST_CASE("NtfsMftList rejects a malformed --log option with usage",
          "[mft][sample][characterization]") {
  const TempTreeImage image;
  const SampleRun run = RunSample({L"--log=bogus", image.path.wstring()});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK_THAT(run.out, ContainsSubstring("Usage: ntfsmftlist"));
}

TEST_CASE("NtfsMftList reports a target that is not an NTFS volume",
          "[mft][sample][characterization]") {
  const SampleRun run = RunSample({L"no-such-volume.img"});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err == missing_volume_error);
}

TEST_CASE("NtfsMftList shows one letter per DOS permission flag",
          "[mft][sample][characterization]") {
  const auto [permission, attributes] = GENERATE(
      table<NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission, std::string_view>({
          {NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::ReadOnly, "R-----"},
          {NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Hidden, "-H----"},
          {NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::System, "--S---"},
          {NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Compressed,
           "---C--"},
          {NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Encrypted,
           "----E-"},
          {NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Sparse, "-----P"},
      }));
  INFO(std::string(attributes));
  std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  NTFS_BROWSER_NAMESPACE::Tests::SetStdInfoPermission(
      image, NTFS_BROWSER_NAMESPACE::Tests::mft_tree_report_idx,
      static_cast<DWORD>(permission));
  const TempTreeImage tree_image(image);
  const SampleRun run = RunSample({tree_image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring(" " + std::string(attributes) +
                                        "  1 \\Docs\\report.txt\n"));
}

TEST_CASE("NtfsMftList reports a file that is not an NTFS volume",
          "[mft][sample][characterization]") {
  const TempTreeImage not_ntfs(std::vector<BYTE>(4096, 0));
  const SampleRun run = RunSample({not_ntfs.path.wstring()});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out.empty());
  CHECK_THAT(run.err,
             ContainsSubstring("Cannot open " + not_ntfs.path.string() +
                               " as an NTFS volume\n"));
}

TEST_CASE("NtfsMftList usage names the device and image forms",
          "[mft][sample][characterization]") {
  const SampleRun run = RunSample({L"--deleted"});

#ifdef _WIN32
  CHECK_THAT(run.out, ContainsSubstring("eg. ntfsmftlist c:\n"));
#else
  CHECK_THAT(run.out, ContainsSubstring("eg. ntfsmftlist /dev/sda1\n"));
  CHECK_THAT(run.out,
             ContainsSubstring("eg. ntfsmftlist --deleted ./disk.img\n"));
#endif
}

#ifndef _WIN32

TEST_CASE("NtfsMftList refuses a directory after the image",
          "[mft][sample][characterization]") {
  const TempTreeImage image;
  const SampleRun run = RunSample({image.path.wstring() + L"/Docs"});

  CHECK(run.exit_code == sample_failure_exit_code);
  CHECK(run.out.empty());
  CHECK_THAT(run.err, ContainsSubstring("as an NTFS volume\n"));
}

#endif  // !_WIN32

namespace {

// Slots of the busy image the thread tests list: past three chunks of the
// listing, and past two windows of the scan.
constexpr ULONGLONG busy_slot_count = 8300;

// Clusters in each fragment of that image, the most one length byte holds.
constexpr ULONGLONG busy_fragment_clusters = 255;

}  // namespace

TEST_CASE("NtfsMftList lists the same with one thread as with several",
          "[mft][sample][threads]") {
  const TempTreeImage image(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithBusyMftScan(
          busy_slot_count, busy_fragment_clusters));
  const bool deleted = GENERATE(false, true);
  const bool recover = GENERATE(false, true);
  std::vector<std::wstring> options;
  if (deleted) {
    options.emplace_back(L"--deleted");
  }
  if (recover) {
    options.emplace_back(L"--recover");
  }
  const auto run_with = [&](const std::wstring& threads) {
    std::vector<std::wstring> args = options;
    args.push_back(threads);
    args.push_back(image.path.wstring());
    return RunSample(args);
  };

  const SampleRun one = run_with(L"--threads=1");
  const SampleRun two = run_with(L"--threads=2");
  const SampleRun many = run_with(L"--threads=8");
  const SampleRun four = run_with(L"--threads=4");

  INFO("deleted " << deleted << " recover " << recover);
  REQUIRE(one.exit_code == 0);
  CHECK(one.out.size() > 100000);
  CHECK(two.exit_code == 0);
  CHECK(two.out == one.out);
  CHECK(many.exit_code == 0);
  CHECK(many.out == one.out);
  CHECK(four.exit_code == 0);
  CHECK(four.out == one.out);
}

TEST_CASE("NtfsMftList with several threads lists the tree image as with one",
          "[mft][sample][threads]") {
  const TempTreeImage image;

  const SampleRun live = RunSample({L"--threads=4", image.path.wstring()});
  const SampleRun deleted =
      RunSample({L"--threads=4", L"--deleted", image.path.wstring()});

  CHECK(live.exit_code == 0);
  CHECK(live.out == WithLocalTimes(live_listing));
  CHECK(deleted.exit_code == 0);
  CHECK(deleted.out == WithLocalTimes(deleted_listing));
}

TEST_CASE("NtfsMftList with several threads keeps the paths under a loop",
          "[mft][sample][threads]") {
  std::vector<BYTE> tree =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree();
  NTFS_BROWSER_NAMESPACE::Tests::MftTreeParents::LoopDocsAndNewDir(tree);
  const TempTreeImage image(tree);

  const SampleRun one = RunSample({L"--threads=1", image.path.wstring()});
  const SampleRun many = RunSample({L"--threads=4", image.path.wstring()});

  CHECK(many.exit_code == 0);
  CHECK(many.out == one.out);
  CHECK_THAT(many.out, ContainsSubstring("  1 <lost #16>\\NewDir\\Docs\n"));
}

TEST_CASE("NtfsMftList rejects a thread count that is not a number from 1 up",
          "[mft][sample][threads]") {
  const TempTreeImage image;
  for (const std::wstring_view option :
       {L"--threads=", L"--threads=0", L"--threads=-1", L"--threads=two",
        L"--threads=5000", L"--threads=1x", L"--threads=00000"}) {
    const SampleRun run =
        RunSample({std::wstring(option), image.path.wstring()});

    INFO(Fs::path(std::wstring(option)).string());
    CHECK(run.exit_code == sample_failure_exit_code);
    CHECK_THAT(run.out, ContainsSubstring("Usage: ntfsmftlist"));
  }
}

TEST_CASE("NtfsMftList usage names the --threads option",
          "[mft][sample][threads]") {
  const SampleRun run = RunSample({L"--deleted"});

  CHECK_THAT(run.out, ContainsSubstring("[--threads=N]"));
  CHECK_THAT(run.out, ContainsSubstring("--threads=N   threads that parse"));
}
