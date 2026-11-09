#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/example/ntfsdump/ntfsdump-core.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/io/ntfsdir-sample-support.h>
#include <ntfs-browser/test/support/sample-run.h>

#include "cache/strategy.h"
#include "ntfs-volume.h"
#include "sample-dump-support.h"

namespace TestIo = NTFS_BROWSER_NAMESPACE::Test::Io;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace ExampleNtfsDump = NTFS_BROWSER_NAMESPACE::Example::NtfsDump;

namespace {

using NoCacheVolume = NTFS_BROWSER_NAMESPACE::NtfsVolume<
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>;
using ExampleNtfsDump::DumpResult;
using NTFS_BROWSER_NAMESPACE::Test::Io::DumpTreeImage;

// A volume over image, over an in-memory image.
std::unique_ptr<NoCacheVolume>
    OpenVolume(std::vector<BYTE> image,
               NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  return std::make_unique<NoCacheVolume>(
      std::make_unique<
          NTFS_BROWSER_NAMESPACE::Test::DiskReaders::MemoryDiskReader>(
          std::move(image)),
      options);
}

// Dumps path from a fresh volume over image.
DumpResult DumpPath(std::vector<BYTE> image, std::wstring_view path,
                    NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  const std::unique_ptr<NoCacheVolume> volume =
      OpenVolume(std::move(image), options);
  return ExampleNtfsDump::DumpFile(*volume, path);
}

// The in-use file of the dump tree, with resident data.
constexpr std::wstring_view report_path = L"Docs\\report.txt";

// The non-resident file of the dump tree, past 16 KiB.
constexpr std::wstring_view big_path = L"big.bin";

// Text column of one dump row. A byte outside printable ASCII shows as '.'.
std::wstring TextColumn(const std::vector<unsigned>& bytes) {
  std::wstring text;
  for (const unsigned byte : bytes) {
    text += static_cast<wchar_t>(byte >= 0x20 && byte < 0x7F ? byte : '.');
  }
  return text;
}

}  // namespace

TEST_CASE("NtfsDump refuses a volume that is not NTFS",
          "[ntfsdump][characterization]") {
  const DumpResult result =
      DumpPath(std::vector<BYTE>(64 * 1024, 0), report_path);

  REQUIRE(result.message == L"Not a valid NTFS volume or NTFS version < 3.0");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reports a root directory record that does not read",
          "[ntfsdump][characterization]") {
  const DumpResult result =
      DumpPath(NTFS_BROWSER_NAMESPACE::Test::FakeImage::
                   BuildFakeNtfsImageWithCorruptRootRecord(),
               report_path);

  REQUIRE(result.message == L"Cannot read root directory of volume");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reports root attributes that do not parse",
          "[ntfsdump][characterization]") {
  const DumpResult result =
      DumpPath(NTFS_BROWSER_NAMESPACE::Test::FakeImage::
                   BuildFakeNtfsImageWithAttrOffsetOutOfBounds(),
               report_path);

  REQUIRE(result.message == L"Cannot parse attributes");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reports a path entry that is not in its directory",
          "[ntfsdump][characterization]") {
  SECTION("in the last component") {
    const DumpResult result = DumpPath(DumpTreeImage(), L"nope.txt");

    REQUIRE(result.message == L"File not found");
    REQUIRE(result.dump.empty());
  }

  SECTION("in a directory on the path") {
    const DumpResult result = DumpPath(DumpTreeImage(), L"nope\\report.txt");

    REQUIRE(result.message == L"File not found");
    REQUIRE(result.dump.empty());
  }
}

TEST_CASE("NtfsDump reports a directory on the path that does not read",
          "[ntfsdump][characterization]") {
  SECTION("strict hides the damaged directory") {
    const DumpResult result =
        DumpPath(DumpTreeImage(), L"Docs\\Broken\\report.txt");

    REQUIRE(result.message == L"File not found");
    REQUIRE(result.dump.empty());
  }

  SECTION("recovering reads the directory that does not read") {
    // The sample words this message as "root directory" here too.
    const DumpResult result = DumpPath(
        DumpTreeImage(), L"Docs\\Broken\\report.txt", {.recover_errors = true});

    REQUIRE(result.message == L"Cannot read root directory of volume");
    REQUIRE(result.dump.empty());
  }
}

TEST_CASE("NtfsDump reports a freed directory on the path as not found",
          "[ntfsdump][characterization]") {
  // OldDir is freed, and a default lookup hides freed names.
  const DumpResult result =
      DumpPath(DumpTreeImage(), L"Docs\\OldDir\\draft.doc");

  REQUIRE(result.message == L"File not found");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reports a final entry that does not read",
          "[ntfsdump][characterization]") {
  SECTION("strict hides the damaged entry") {
    const DumpResult result = DumpPath(DumpTreeImage(), L"Docs\\Broken");

    REQUIRE(result.message == L"File not found");
    REQUIRE(result.dump.empty());
  }

  SECTION("recovering reads the entry that does not read") {
    const DumpResult result =
        DumpPath(DumpTreeImage(), L"Docs\\Broken", {.recover_errors = true});

    REQUIRE(result.message == L"Cannot read file");
    REQUIRE(result.dump.empty());
  }
}

TEST_CASE("NtfsDump reports a freed final file as not found",
          "[ntfsdump][characterization]") {
  // old.tmp is freed, so a default lookup hides it.
  const DumpResult result = DumpPath(DumpTreeImage(), L"Docs\\old.tmp");

  REQUIRE(result.message == L"File not found");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reports a data run that cannot be read",
          "[ntfsdump][characterization]") {
  // Clusters past the end of the image, so the read fails.
  const DumpResult result = DumpPath(
      NTFS_BROWSER_NAMESPACE::Test::FakeImage::BuildFakeNtfsImageWithDumpTree(
          NTFS_BROWSER_NAMESPACE::Test::FakeImage::dump_big_file_size,
          {{.lcn = 1000, .clusters = 20}}),
      big_path);

  REQUIRE(result.message == L"Read data error");
  REQUIRE(result.dump.empty());
}

TEST_CASE(
    "NtfsDump reports a data read that returns fewer bytes than the "
    "file holds",
    "[ntfsdump][characterization]") {
  // One cluster mapped for a file of 20000 bytes: the read comes back short.
  const DumpResult result = DumpPath(
      NTFS_BROWSER_NAMESPACE::Test::FakeImage::BuildFakeNtfsImageWithDumpTree(
          NTFS_BROWSER_NAMESPACE::Test::FakeImage::dump_big_file_size,
          {{.lcn = 40, .clusters = 1}}),
      big_path);

  REQUIRE(result.message == L"Read data error");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump dumps a resident file, with a short last row",
          "[ntfsdump][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), report_path);

  REQUIRE(result.message.empty());

  // dump_report_content is 19 bytes: a full row, then 3 bytes. The missing
  // bytes of the last row have no hex and no text.
  const std::wstring first_row =
      L"0000    4E 54 46 53 20 64 75 6D - 70 20 74 65 73 74 01 7F   "
      L"NTFS dump test..";
  const std::wstring last_row = L"0010    E9 0D 0A " + std::wstring(12, L' ') +
                                L"   - " + std::wstring(26, L' ') + L"...";
  REQUIRE(result.dump == first_row + L"\n" + last_row + L"\n");
}

TEST_CASE("NtfsDump cuts a file at 16 KiB", "[ntfsdump][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), big_path);

  REQUIRE(result.message.empty());

  // 16384 bytes make 1024 rows. Every row ends in LF.
  std::size_t line_ends = 0;
  for (std::size_t at = result.dump.find(L"\n"); at != std::wstring::npos;
       at = result.dump.find(L"\n", at + 1)) {
    ++line_ends;
  }
  REQUIRE(line_ends == 1024);

  // The file's byte k is k & 0xFF, so the last row holds bytes 0xF0 to 0xFF.
  std::vector<unsigned> last_bytes;
  for (unsigned byte = 0xF0; byte <= 0xFF; ++byte) {
    last_bytes.push_back(byte);
  }
  const std::wstring last_row =
      L"3FF0    F0 F1 F2 F3 F4 F5 F6 F7 - F8 F9 FA FB FC FD FE FF   " +
      TextColumn(last_bytes) + L"\n";
  REQUIRE(result.dump.size() >= last_row.size());
  REQUIRE(result.dump.substr(result.dump.size() - last_row.size()) == last_row);
}

TEST_CASE("NtfsDump dumps nothing for a directory, which has no data stream",
          "[ntfsdump][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), L"Docs");

  REQUIRE(result.message.empty());
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump dumps nothing for a zero-byte file",
          "[ntfsdump][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), L"empty.txt");

  REQUIRE(result.message.empty());
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reads a target, with the options before or after it",
          "[ntfsdump][arguments]") {
  SECTION("the target alone") {
    TestIo::Argv argv{L"disk.img/Docs"};
    const auto args =
        ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data());

    REQUIRE(args.has_value());
    CHECK(args->target == L"disk.img/Docs");
    CHECK_FALSE(args->common.help);
    CHECK_FALSE(args->common.volume_options.include_deleted);
    CHECK_FALSE(args->common.volume_options.recover_errors);
  }

  SECTION("every option, before the target") {
    TestIo::Argv argv{L"--deleted", L"--recover", L"disk.img"};
    const auto args =
        ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data());

    REQUIRE(args.has_value());
    CHECK(args->target == L"disk.img");
    CHECK(args->common.volume_options.include_deleted);
    CHECK(args->common.volume_options.recover_errors);
  }

  SECTION("an option after the target") {
    TestIo::Argv argv{L"disk.img", L"--recover"};
    const auto args =
        ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data());

    REQUIRE(args.has_value());
    CHECK(args->common.volume_options.recover_errors);
  }

  SECTION("a --log option") {
    TestIo::Argv argv{L"--log=console:error", L"disk.img"};

    CHECK(
        ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data()).has_value());
  }
}

TEST_CASE("NtfsDump asks for help without a target", "[ntfsdump][arguments]") {
  for (const std::wstring_view option : {L"--help", L"-h"}) {
    TestIo::Argv argv{option};
    const auto args =
        ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data());

    REQUIRE(args.has_value());
    CHECK(args->common.help);
  }
}

TEST_CASE("NtfsDump refuses a bad command line", "[ntfsdump][arguments]") {
  SECTION("no argument") {
    TestIo::Argv argv{};

    CHECK_FALSE(
        ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data()).has_value());
  }

  SECTION("only options") {
    TestIo::Argv argv{L"--deleted", L"--recover"};

    CHECK_FALSE(
        ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data()).has_value());
  }

  SECTION("two targets") {
    TestIo::Argv argv{L"disk.img", L"other.img"};

    CHECK_FALSE(
        ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data()).has_value());
  }

  SECTION("a malformed --log") {
    TestIo::Argv argv{L"--log=bogus", L"disk.img"};

    CHECK_FALSE(
        ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data()).has_value());
  }
}

TEST_CASE("NtfsDump dumps the file named in an image target",
          "[ntfsdump][run]") {
  const TestIo::TempImage image(DumpTreeImage());
  const std::wstring target = image.path.wstring() + L"/Docs/report.txt";
  TestIo::Argv argv{target};
  const auto args = ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsDump::DumpTarget(*args, out, err);
      });
  const DumpResult expected = DumpPath(DumpTreeImage(), report_path);

  CHECK(run.exit_code == 0);
  CHECK(run.err.empty());
  CHECK(run.out == TestIo::Narrow(expected.dump));
}

TEST_CASE("NtfsDump dumps nothing for the root of an image target",
          "[ntfsdump][run]") {
  const TestIo::TempImage image(DumpTreeImage());
  TestIo::Argv argv{image.path.wstring()};
  const auto args = ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsDump::DumpTarget(*args, out, err);
      });

  CHECK(run.exit_code == 0);
  CHECK(run.err.empty());
  CHECK(run.out.empty());
}

TEST_CASE("NtfsDump reports a missing file on stderr", "[ntfsdump][run]") {
  const TestIo::TempImage image(DumpTreeImage());
  const std::wstring target = image.path.wstring() + L"/Docs/nope.txt";
  TestIo::Argv argv{target};
  const auto args = ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsDump::DumpTarget(*args, out, err);
      });

  CHECK(run.exit_code == ExampleNtfsDump::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err == "File not found\n");
}

TEST_CASE("NtfsDump --deleted finds a deleted file", "[ntfsdump][run]") {
  const TestIo::TempImage image(DumpTreeImage());
  const std::wstring target = image.path.wstring() + L"/Docs/old.tmp";
  TestIo::Argv hidden{target};
  TestIo::Argv shown{L"--deleted", target};
  const auto hidden_args =
      ExampleNtfsDump::ParseArguments(hidden.Count(), hidden.Data());
  const auto shown_args =
      ExampleNtfsDump::ParseArguments(shown.Count(), shown.Data());
  REQUIRE(hidden_args.has_value());
  REQUIRE(shown_args.has_value());

  const TestIo::Captured without =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsDump::DumpTarget(*hidden_args, out, err);
      });
  const TestIo::Captured with =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsDump::DumpTarget(*shown_args, out, err);
      });

  CHECK(without.exit_code == ExampleNtfsDump::failure_exit_code);
  CHECK(with.exit_code == 0);
  CHECK_FALSE(with.out.empty());
}

#ifndef _WIN32

TEST_CASE("NtfsDump reports a target that is not NTFS on stderr",
          "[ntfsdump][run]") {
  const TestIo::TempImage image(std::vector<BYTE>(64 * 1024, 0));
  TestIo::Argv argv{image.path.wstring()};
  const auto args = ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsDump::DumpTarget(*args, out, err);
      });

  CHECK(run.exit_code == ExampleNtfsDump::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err.starts_with("Cannot get NTFS BPB from boot sector of "));
}

TEST_CASE("NtfsDump reports a target that names nothing on stderr",
          "[ntfsdump][run]") {
  TestIo::Argv argv{L"/nonexistent/disk.img/Docs"};
  const auto args = ExampleNtfsDump::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsDump::DumpTarget(*args, out, err);
      });

  CHECK(run.exit_code == ExampleNtfsDump::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err ==
        "Cannot find a device or image file in /nonexistent/disk.img/Docs\n");
}

#endif  // !_WIN32

TEST_CASE("NtfsDump executable prints usage for --help",
          "[ntfsdump][executable]") {
  const std::filesystem::path exe = NTFS_DUMP_EXE;
  REQUIRE(std::filesystem::exists(exe));
  const TestSupport::SampleRun run = TestSupport::RunSample(exe, {L"--help"});

  CHECK(run.exit_code == 0);
  CHECK(run.out == ExampleNtfsDump::UsageText());
  CHECK(run.err.empty());
}

TEST_CASE("NtfsDump executable prints usage on stderr for a bad command line",
          "[ntfsdump][executable]") {
  const std::filesystem::path exe = NTFS_DUMP_EXE;
  REQUIRE(std::filesystem::exists(exe));
  const TestSupport::SampleRun run = TestSupport::RunSample(exe, {});

  CHECK(run.exit_code == ExampleNtfsDump::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err == ExampleNtfsDump::UsageText());
}

TEST_CASE("NtfsDump executable dumps a file of an image on stdout",
          "[ntfsdump][executable]") {
  const std::filesystem::path exe = NTFS_DUMP_EXE;
  REQUIRE(std::filesystem::exists(exe));
  const TestIo::TempImage image(DumpTreeImage());
  const TestSupport::SampleRun run = TestSupport::RunSample(
      exe, {L"--recover", image.path.wstring() + L"/Docs/report.txt"});
  const DumpResult expected = DumpPath(DumpTreeImage(), report_path);

  CHECK(run.exit_code == 0);
  CHECK(run.err.empty());
  CHECK(run.out == TestIo::Narrow(expected.dump));
}

TEST_CASE("NtfsDump executable reports a missing file with a failure code",
          "[ntfsdump][executable]") {
  const std::filesystem::path exe = NTFS_DUMP_EXE;
  REQUIRE(std::filesystem::exists(exe));
  const TestIo::TempImage image(DumpTreeImage());
  const TestSupport::SampleRun run =
      TestSupport::RunSample(exe, {image.path.wstring() + L"/Docs/nope.txt"});

  CHECK(run.exit_code == ExampleNtfsDump::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err.ends_with("File not found\n"));
}
