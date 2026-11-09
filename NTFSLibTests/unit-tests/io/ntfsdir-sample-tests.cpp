#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/example/ntfsdir/arguments.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>
#include <ntfs-browser/test/support/sample-run.h>

#include "win-types.h"

#ifndef _WIN32
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace ExampleNtfsDir = NTFS_BROWSER_NAMESPACE::Example::NtfsDir;
using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Test::FakeImage::dump_broken_alter_time;
using NTFS_BROWSER_NAMESPACE::Test::Io::Below;
using NTFS_BROWSER_NAMESPACE::Test::Io::DirectoryHeader;
using NTFS_BROWSER_NAMESPACE::Test::Io::DocsListing;
using NTFS_BROWSER_NAMESPACE::Test::Io::InImage;
using NTFS_BROWSER_NAMESPACE::Test::Io::ListingLine;
using NTFS_BROWSER_NAMESPACE::Test::Io::RootListing;
using NTFS_BROWSER_NAMESPACE::Test::Io::RunDir;
using NTFS_BROWSER_NAMESPACE::Test::Io::TempImage;
using NTFS_BROWSER_NAMESPACE::Test::Support::SampleRun;
namespace Fs = std::filesystem;

namespace {

// The exit code of a run that fails.
constexpr int failure = ExampleNtfsDir::failure_exit_code;

// A path argument that opens nothing, for the tests that stop at parsing.
#ifdef _WIN32
constexpr std::wstring_view any_target = L"c:\\";
#else
constexpr std::wstring_view any_target = L"/dev/sda1";
#endif

// Lists the root by folder: with Auto, a recursive listing of a root is read
// in $MFT order.
const std::wstring by_folder = L"--engine=fullcache";

}  // namespace

TEST_CASE("NtfsDir prints the reason and the usage without arguments",
          "[io][sample][ntfsdir]") {
  const SampleRun run = RunDir({});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK_THAT(run.err, ContainsSubstring("ntfsdir: A target is needed\n"));
  CHECK_THAT(run.err, ContainsSubstring(ExampleNtfsDir::UsageText()));
}

TEST_CASE("NtfsDir prints the usage for two targets", "[io][sample][ntfsdir]") {
  const SampleRun run = RunDir({std::wstring(any_target), L"/other"});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK_THAT(run.err,
             ContainsSubstring("ntfsdir: Only one target is allowed\n"));
  CHECK_THAT(run.err, ContainsSubstring("Usage: ntfsdir"));
}

TEST_CASE("NtfsDir prints the usage for a malformed --log option",
          "[io][sample][ntfsdir]") {
  const SampleRun run = RunDir({L"--log=bogus", std::wstring(any_target)});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK_THAT(run.err, ContainsSubstring("Malformed option --log=bogus"));
}

TEST_CASE("NtfsDir prints the usage for an engine it does not know",
          "[io][sample][ntfsdir]") {
  const SampleRun run = RunDir({L"--engine=turbo", std::wstring(any_target)});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK_THAT(run.err, ContainsSubstring("Unknown engine \"turbo\""));
}

TEST_CASE("NtfsDir --help prints the usage to stdout",
          "[io][sample][ntfsdir]") {
  const SampleRun run = RunDir({L"--help"});

  CHECK(run.exit_code == 0);
  CHECK(run.out == ExampleNtfsDir::UsageText());
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir usage names the form of the platform",
          "[io][sample][ntfsdir]") {
  const SampleRun run = RunDir({L"--help"});

#ifdef _WIN32
  CHECK_THAT(run.out, ContainsSubstring("eg. ntfsdir c:\n"));
#else
  CHECK_THAT(run.out, ContainsSubstring("eg. ntfsdir /dev/sda1\n"));
  CHECK_THAT(run.out, ContainsSubstring("eg. ntfsdir ./disk.img/Windows"));
#endif
}

TEST_CASE("NtfsDir takes --recover without changing a healthy listing",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun run = RunDir({L"--recover", InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("report.txt"));
}

TEST_CASE("NtfsDir --deleted adds a marker column to a listing",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun run = RunDir({InImage(image.path, L"Docs"), L"--deleted"});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("report.txt", " "));
}

TEST_CASE("NtfsDir --recursive lists every folder under a header",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const std::string root = image.path.string();
  const std::wstring engine =
      GENERATE(L"--engine=fullcache", L"--engine=nocache");
  const std::wstring option = GENERATE(L"--recursive", L"-r");
  INFO(Fs::path(engine).string() << " " << Fs::path(option).string());

  const SampleRun run = RunDir({engine, option, image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out ==
        DirectoryHeader(root) + ListingLine("Docs", true, 0) +
            DirectoryHeader(Below(root, "Docs")) +
            ListingLine("report.txt", false, dump_broken_alter_time) +
            "Files: 1, Directories: 1\n");
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir --recursive starts at the folder it is given",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun run = RunDir({L"-r", InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK_THAT(run.out, ContainsSubstring("\nDirectory of "));
  CHECK_THAT(run.out, ContainsSubstring("Docs\n"));
  CHECK_THAT(run.out, ContainsSubstring("report.txt\n"));
  CHECK_THAT(run.out, ContainsSubstring("Files: 1, Directories: 0\n"));
}

TEST_CASE("NtfsDir --recursive enters a folder once when an entry loops",
          "[io][sample][ntfsdir]") {
  const TempImage image(NTFS_BROWSER_NAMESPACE::Test::FakeImage::
                            BuildFakeNtfsImageWithDirectoryLoop());
  const std::string root = image.path.string();
  const std::string docs = Below(root, "Docs");
  const std::string sub = Below(docs, "Sub");
  const SampleRun run =
      RunDir({by_folder, L"--recursive", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DirectoryHeader(root) + ListingLine("Docs", true, 0) +
                       DirectoryHeader(docs) + ListingLine("Sub", true, 0) +
                       DirectoryHeader(sub) + ListingLine("Back", true, 0) +
                       "Files: 0, Directories: 3\n");
  CHECK_THAT(run.err,
             ContainsSubstring(
                 "Skipping \\Docs\\Sub\\Back: directory already listed"));
}

TEST_CASE("NtfsDir --recursive --recover skips a stale entry that loops",
          "[io][sample][ntfsdir]") {
  const TempImage image(NTFS_BROWSER_NAMESPACE::Test::FakeImage::
                            BuildFakeNtfsImageWithOrphanedDirectoryLoop());
  const std::string root = image.path.string();
  const std::string docs = Below(root, "Docs");
  const std::string sub = Below(docs, "Sub");
  const SampleRun run =
      RunDir({by_folder, L"--recursive", L"--recover", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DirectoryHeader(root) + ListingLine("Docs", true, 0) +
                       DirectoryHeader(docs) + ListingLine("Sub", true, 0) +
                       DirectoryHeader(sub) + ListingLine("Docs", true, 0) +
                       "Files: 0, Directories: 3\n");
  CHECK_THAT(run.err,
             ContainsSubstring(
                 "Skipping \\Docs\\Sub\\Docs: directory already listed"));
}

TEST_CASE("NtfsDir --recursive does not show a stale entry without --recover",
          "[io][sample][ntfsdir]") {
  const TempImage image(NTFS_BROWSER_NAMESPACE::Test::FakeImage::
                            BuildFakeNtfsImageWithOrphanedDirectoryLoop());
  const std::string root = image.path.string();
  const std::string docs = Below(root, "Docs");
  const SampleRun run =
      RunDir({by_folder, L"--recursive", image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DirectoryHeader(root) + ListingLine("Docs", true, 0) +
                       DirectoryHeader(docs) + ListingLine("Sub", true, 0) +
                       "Files: 0, Directories: 2\n");
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir lists the root of an image file by folder",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const std::wstring engine =
      GENERATE(L"--engine=fullcache", L"--engine=nocache");
  const SampleRun run = RunDir({engine, image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == RootListing());
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir with Auto lists the root by folder unless it is recursive",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun root = RunDir({image.path.wstring()});

  CHECK(root.exit_code == 0);
  CHECK(root.out == RootListing());
  CHECK(root.err.empty());
}

TEST_CASE("NtfsDir --recursive reads a whole volume in $MFT order with Auto",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun automatic = RunDir({L"-r", image.path.wstring()});
  const SampleRun mft = RunDir({L"--engine=mftlist", image.path.wstring()});

  CHECK(automatic.exit_code == 0);
  CHECK_THAT(automatic.out, ContainsSubstring("\t\\Docs\n"));
  CHECK_THAT(automatic.out, ContainsSubstring("\t\\Docs\\report.txt\n"));
  CHECK_THAT(automatic.out, ContainsSubstring("Files: 3, Directories: 2\n"));
  CHECK_THAT(automatic.out, !ContainsSubstring("Directory of"));
  CHECK(automatic.out == mft.out);
  CHECK(automatic.err.empty());
}

TEST_CASE("NtfsDir lists a folder of an image file", "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun run = RunDir({InImage(image.path, L"Docs")});

  CHECK(run.exit_code == 0);
  CHECK(run.out == DocsListing("report.txt"));
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir finds a folder whatever its case", "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun exact = RunDir({InImage(image.path, L"Docs")});
  const SampleRun lower = RunDir({InImage(image.path, L"docs")});
  const SampleRun slash = RunDir({InImage(image.path, L"DOCS/")});

  CHECK(exact.out == DocsListing("report.txt"));
  CHECK(lower.exit_code == 0);
  CHECK(lower.out == exact.out);
  CHECK(slash.exit_code == 0);
  CHECK(slash.out == exact.out);
}

TEST_CASE("NtfsDir accepts --log before or after the image",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun before =
      RunDir({L"--log=console:off", by_folder, image.path.wstring()});
  const SampleRun after =
      RunDir({by_folder, image.path.wstring(), L"--log=console:off"});

  CHECK(before.exit_code == 0);
  CHECK(before.out == RootListing());
  CHECK(after.exit_code == before.exit_code);
  CHECK(after.out == before.out);
}

TEST_CASE("NtfsDir reports a folder that does not exist",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun run = RunDir({InImage(image.path, L"Nope")});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK_THAT(run.err, ContainsSubstring("Cannot find directory Nope"));
}

TEST_CASE("NtfsDir reports a file where a folder is wanted",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const SampleRun run = RunDir({InImage(image.path, L"Docs/report.txt")});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK_THAT(run.err, ContainsSubstring("report.txt is not a directory"));
}

TEST_CASE("NtfsDir reports an image that is not an NTFS volume",
          "[io][sample][ntfsdir]") {
  const TempImage image(std::vector<BYTE>(4096, 0));
  const SampleRun loud = RunDir({image.path.wstring()});
  const SampleRun quiet = RunDir({L"--log=console:off", image.path.wstring()});

  CHECK(loud.exit_code == failure);
  CHECK(loud.out.empty());
  CHECK_THAT(loud.err, ContainsSubstring("Cannot get NTFS BPB from boot sector "
                                         "of " +
                                         image.path.string()));
  CHECK(quiet.exit_code == failure);
  CHECK(quiet.out.empty());
  CHECK(quiet.err.empty());
}

#ifdef _WIN32

namespace {

// A fresh temp path with the given extension, not yet created.
Fs::path TempPath(std::string_view extension) {
  std::random_device random_device;
  return Fs::temp_directory_path() /
         ("ntfsdir-sample-" + std::to_string(random_device()) +
          std::string(extension));
}

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

// A path on drive letter, in the form the sample takes: "X:" then rest.
std::wstring OnDrive(char letter, std::wstring_view rest) {
  return std::wstring(1, static_cast<wchar_t>(letter)) + L":" +
         std::wstring(rest);
}

// What the library says when it cannot open a volume.
const std::string volume_open_error = "Cannot open volume";

}  // namespace

TEST_CASE("NtfsDir reports a drive letter with no volume",
          "[io][sample][ntfsdir]") {
  const char letter = RequireFreeDriveLetter();
  const SampleRun run = RunDir({OnDrive(letter, L"\\Docs\\sub")});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK_THAT(run.err, ContainsSubstring(volume_open_error));
}

TEST_CASE("NtfsDir --log=console:off silences the library line",
          "[io][sample][ntfsdir]") {
  const char letter = RequireFreeDriveLetter();
  const SampleRun run = RunDir({L"--log=console:off", OnDrive(letter, L"\\")});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDir accepts --log before or after the path",
          "[io][sample][ntfsdir]") {
  const char letter = RequireFreeDriveLetter();
  const std::wstring path = OnDrive(letter, L"\\");
  const SampleRun before = RunDir({L"--log=console:info", path});
  const SampleRun after = RunDir({path, L"--log=console:info"});

  CHECK(before.exit_code == failure);
  CHECK_THAT(before.err, ContainsSubstring(volume_open_error));
  CHECK(after.exit_code == before.exit_code);
  CHECK(after.out == before.out);
  CHECK_THAT(after.err, ContainsSubstring(volume_open_error));
}

TEST_CASE("NtfsDir --log=file keeps the library line in the file",
          "[io][sample][ntfsdir]") {
  const char letter = RequireFreeDriveLetter();
  const Fs::path log_path = TempPath(".log");
  const SampleRun run = RunDir(
      {L"--log=file:trace:" + log_path.wstring(), OnDrive(letter, L"\\")});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  REQUIRE(Fs::exists(log_path));
  std::ifstream input(log_path);
  const std::string log_text((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  CHECK_THAT(log_text, ContainsSubstring("[error] Cannot open volume"));
  std::error_code ignored;
  Fs::remove(log_path, ignored);
}

TEST_CASE("NtfsDir reports a log path it cannot open and goes on",
          "[io][sample][ntfsdir]") {
  const char letter = RequireFreeDriveLetter();
  const Fs::path log_dir = Fs::temp_directory_path();
  const SampleRun run = RunDir(
      {L"--log=file:trace:" + log_dir.wstring(), OnDrive(letter, L"\\")});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK_THAT(run.err,
             ContainsSubstring("Cannot open log file " + log_dir.string()));
  CHECK_THAT(run.err, ContainsSubstring(volume_open_error));
}

#else

TEST_CASE("NtfsDir reports a path that holds no device or image",
          "[io][sample][ntfsdir]") {
  const SampleRun missing = RunDir({L"/no/such/device"});
  const SampleRun empty = RunDir({L""});
  const SampleRun relative = RunDir({L"windows"});

  CHECK(missing.exit_code == failure);
  CHECK(missing.out.empty());
  CHECK_THAT(missing.err,
             ContainsSubstring(
                 "Cannot find a device or image file in /no/such/device"));
  CHECK(empty.exit_code == failure);
  CHECK_THAT(empty.err,
             ContainsSubstring("Cannot find a device or image file in "));
  CHECK(relative.exit_code == failure);
  CHECK_THAT(
      relative.err,
      ContainsSubstring("Cannot find a device or image file in windows"));
}

TEST_CASE("NtfsDir reports a device it may not read", "[io][sample][ntfsdir]") {
  if (geteuid() == 0) {
    SKIP("root can read any file");
  }
  const TempImage image;
  REQUIRE(chmod(image.path.c_str(), 0) == 0);
  const SampleRun run = RunDir({image.path.wstring()});

  CHECK(run.exit_code == failure);
  CHECK(run.out.empty());
  CHECK_THAT(run.err,
             ContainsSubstring("Cannot read device " + image.path.string() +
                               ": read access is needed"));
}

TEST_CASE("NtfsDir goes on after a log path it cannot open",
          "[io][sample][ntfsdir]") {
  const TempImage image;
  const Fs::path log_dir = Fs::temp_directory_path();
  const SampleRun run = RunDir({L"--log=file:trace:" + log_dir.wstring(),
                                by_folder, image.path.wstring()});

  CHECK(run.exit_code == 0);
  CHECK(run.out == RootListing());
  CHECK_THAT(run.err, ContainsSubstring("Cannot open log file " +
                                        log_dir.string() + "\n"));
}

#endif
