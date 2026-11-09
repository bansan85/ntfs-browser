#include <array>
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

#include <ntfs-browser/example/ntfsattr/ntfsattr-core.h>
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
namespace ExampleNtfsAttr = NTFS_BROWSER_NAMESPACE::Example::NtfsAttr;

namespace {

using FullCacheVolume = NTFS_BROWSER_NAMESPACE::NtfsVolume<
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>;
using ExampleNtfsAttr::DumpResult;
using NTFS_BROWSER_NAMESPACE::Test::Io::DumpTreeImage;

// A volume over image, over an in-memory image.
std::unique_ptr<FullCacheVolume>
    OpenVolume(std::vector<BYTE> image,
               NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  return std::make_unique<FullCacheVolume>(
      std::make_unique<
          NTFS_BROWSER_NAMESPACE::Test::DiskReaders::MemoryDiskReader>(
          std::move(image)),
      options);
}

// Dumps path from a fresh volume over image.
DumpResult DumpPath(std::vector<BYTE> image, std::wstring_view path,
                    NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  const std::unique_ptr<FullCacheVolume> volume =
      OpenVolume(std::move(image), options);
  return ExampleNtfsAttr::DumpAttributes(*volume, path);
}

// The in-use file of the dump tree, with resident data.
constexpr std::wstring_view report_path = L"Docs\\report.txt";

// Position just past the heading line name in dump, searched from from. npos
// when dump has no such heading after from.
std::size_t FindHeading(const std::wstring& dump, std::wstring_view name,
                        std::size_t from) {
  const std::wstring heading = std::wstring(name) + L"\n";
  std::size_t at = dump.find(heading, from);
  while (at != std::wstring::npos && at != 0 && dump[at - 1] != L'\n') {
    at = dump.find(heading, at + 1);
  }
  return at == std::wstring::npos ? at : at + heading.size();
}

}  // namespace

TEST_CASE("NtfsAttr target path keeps the picked file, or its folder",
          "[ntfsattr][characterization]") {
  SECTION("a file is dumped as picked") {
    REQUIRE(ExampleNtfsAttr::TargetPath(report_path, false) == report_path);
  }

  SECTION("a folder is the directory holding the picked file") {
    REQUIRE(ExampleNtfsAttr::TargetPath(report_path, true) == L"Docs");
  }

  SECTION("a file in the root gives the root") {
    REQUIRE(ExampleNtfsAttr::TargetPath(L"big.bin", true).empty());
  }

  SECTION("the root gives the root") {
    REQUIRE(ExampleNtfsAttr::TargetPath(L"", true).empty());
  }

  SECTION("both separators split the parts") {
    REQUIRE(ExampleNtfsAttr::TargetPath(L"a/b\\c", true) == L"a\\b");
  }
}

TEST_CASE("NtfsAttr refuses a volume that is not NTFS",
          "[ntfsattr][characterization]") {
  const DumpResult result =
      DumpPath(std::vector<BYTE>(64 * 1024, 0), report_path);

  REQUIRE(result.message == L"Not a valid NTFS volume or NTFS version < 3.0");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports a root directory record that does not read",
          "[ntfsattr][characterization]") {
  const DumpResult result =
      DumpPath(NTFS_BROWSER_NAMESPACE::Test::FakeImage::
                   BuildFakeNtfsImageWithCorruptRootRecord(),
               report_path);

  REQUIRE(result.message == L"Cannot read root directory of volume");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports root attributes that do not parse",
          "[ntfsattr][characterization]") {
  const DumpResult result =
      DumpPath(NTFS_BROWSER_NAMESPACE::Test::FakeImage::
                   BuildFakeNtfsImageWithAttrOffsetOutOfBounds(),
               report_path);

  REQUIRE(result.message == L"Cannot parse attributes");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports a path entry that is not in its directory",
          "[ntfsattr][characterization]") {
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

TEST_CASE("NtfsAttr reports a directory on the path that does not read",
          "[ntfsattr][characterization]") {
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

TEST_CASE("NtfsAttr reports a freed directory on the path as not found",
          "[ntfsattr][characterization]") {
  // OldDir is freed, and a default lookup hides freed names.
  const DumpResult result =
      DumpPath(DumpTreeImage(), L"Docs\\OldDir\\draft.doc");

  REQUIRE(result.message == L"File not found");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports a final entry that does not read",
          "[ntfsattr][characterization]") {
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

TEST_CASE("NtfsAttr reports a freed final file as not found",
          "[ntfsattr][characterization]") {
  // old.tmp is freed, so a default lookup hides it.
  const DumpResult result = DumpPath(DumpTreeImage(), L"Docs\\old.tmp");

  REQUIRE(result.message == L"File not found");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr dumps every attribute of a file, in record order",
          "[ntfsattr][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), report_path);

  REQUIRE(result.message.empty());

  // Each heading is the attribute name, and each dump row ends in LF.
  const std::size_t standard_info =
      FindHeading(result.dump, L"STANDARD_INFORMATION", 0);
  REQUIRE(standard_info != std::wstring::npos);
  const std::size_t file_name = FindHeading(result.dump, L"FILE_NAME", 0);
  REQUIRE(file_name > standard_info);
  const std::size_t data = FindHeading(result.dump, L"DATA", file_name);
  REQUIRE(data != std::wstring::npos);
  REQUIRE(result.dump.starts_with(L"STANDARD_INFORMATION\n"));

  // The resident $DATA holds dump_report_content and pads to 48 bytes. Its
  // last row is bytes 32 to 47: the content's tail, then five zeros.
  const std::wstring last_row =
      L"70 20 74 65 73 74 01 7F - E9 0D 0A 00 00 00 00 00   "
      L"p test" +
      std::wstring(10, L'.');
  REQUIRE(result.dump.ends_with(last_row + L"\n"));
}

TEST_CASE("NtfsAttr dumps a folder's attributes, index included",
          "[ntfsattr][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), L"Docs");

  REQUIRE(result.message.empty());
  REQUIRE(result.dump.starts_with(L"STANDARD_INFORMATION\n"));
  REQUIRE(FindHeading(result.dump, L"INDEX_ROOT", 0) != std::wstring::npos);
}

TEST_CASE("NtfsAttr dumps the root directory for the empty path",
          "[ntfsattr][characterization]") {
  // The root lists itself as ".", so the bare drive resolves to the root.
  const DumpResult result = DumpPath(DumpTreeImage(), L"");

  REQUIRE(result.message.empty());
  REQUIRE(result.dump.starts_with(L"STANDARD_INFORMATION\n"));
  REQUIRE(FindHeading(result.dump, L"INDEX_ROOT", 0) != std::wstring::npos);
}

TEST_CASE("NtfsAttr folder mode dumps the folder holding the picked file",
          "[ntfsattr][characterization]") {
  const auto volume = OpenVolume(DumpTreeImage());
  const DumpResult by_folder = ExampleNtfsAttr::DumpAttributes(
      *volume, ExampleNtfsAttr::TargetPath(report_path, true));
  const DumpResult by_folder_name =
      ExampleNtfsAttr::DumpAttributes(*volume, L"Docs");

  REQUIRE(by_folder.message.empty());
  REQUIRE(by_folder.dump == by_folder_name.dump);
}

TEST_CASE("NtfsAttr dump of zero bytes has no rows",
          "[ntfsattr][characterization]") {
  const std::vector<BYTE> bytes(16, 0xAA);
  std::wstring lines;

  ExampleNtfsAttr::AppendData(lines, bytes.data(), 0);

  REQUIRE(lines.empty());
}

TEST_CASE("NtfsAttr dump rows split at 16 bytes, last row padded with blanks",
          "[ntfsattr][characterization]") {
  std::vector<BYTE> bytes(17, 0x41);
  bytes[16] = 0x42;
  std::wstring lines;

  ExampleNtfsAttr::AppendData(lines, bytes.data(), 17);

  // The missing bytes have no hex and no text.
  REQUIRE(lines ==
          L"41 41 41 41 41 41 41 41 - 41 41 41 41 41 41 41 41   "
          L"AAAAAAAAAAAAAAAA\n" +
              std::wstring(L"42") + std::wstring(19, L' ') + L"   - " +
              std::wstring(26, L' ') + L"B\n");
}

TEST_CASE("NtfsAttr names each known attribute type",
          "[ntfsattr][characterization]") {
  using NTFS_BROWSER_NAMESPACE::Attr::Type;
  const std::array<std::pair<Type, std::wstring_view>, 16> names = {{
      {Type::StandardInformation, L"STANDARD_INFORMATION"},
      {Type::AttributeList, L"ATTRIBUTE_LIST"},
      {Type::FileName, L"FILE_NAME"},
      {Type::ObjectId, L"OBJECT_ID"},
      {Type::SecurityDescriptor, L"SECURITY_DESCRIPTOR"},
      {Type::VolumeName, L"VOLUME_NAME"},
      {Type::VolumeInformation, L"VOLUME_INFORMATION"},
      {Type::Data, L"DATA"},
      {Type::IndexRoot, L"INDEX_ROOT"},
      {Type::IndexAllocation, L"INDEX_ALLOCATION"},
      {Type::Bitmap, L"BITMAP"},
      {Type::ReparsePoint, L"REPARSE_POINT"},
      {Type::EaInformation, L"EA_INFORMATION"},
      {Type::Ea, L"EA"},
      {Type::PropertySet, L"NULL"},
      {Type::LoggedUtilityStream, L"LOGGED_UTILITY_STREAM"},
  }};

  for (const auto& [type, name] : names) {
    REQUIRE(ExampleNtfsAttr::AttrTypeName(type) == name);
  }
}

TEST_CASE("NtfsAttr names a type with no name by its value",
          "[ntfsattr][characterization]") {
  using NTFS_BROWSER_NAMESPACE::Attr::Type;
  // 0 and 0x8 sit below the named range, 0x110 and 0x1000 above it. 0xFFFFFFFF
  // is the end marker value.
  const std::array<std::pair<DWORD, std::wstring_view>, 5> unnamed = {{
      {0x0U, L"0x0000"},
      {0x8U, L"0x0008"},
      {0x110U, L"0x0110"},
      {0x1000U, L"0x1000"},
      {0xFFFFFFFFU, L"0xFFFFFFFF"},
  }};

  for (const auto& [raw, label] : unnamed) {
    REQUIRE(ExampleNtfsAttr::AttrTypeName(static_cast<Type>(raw)) == label);
  }
}

TEST_CASE("NtfsAttr reads a target, with the options before or after it",
          "[ntfsattr][arguments]") {
  SECTION("the target alone") {
    TestIo::Argv argv{L"disk.img/Docs"};
    const auto args =
        ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());

    REQUIRE(args.has_value());
    CHECK(args->target == L"disk.img/Docs");
    CHECK_FALSE(args->directory);
    CHECK_FALSE(args->common.help);
    CHECK_FALSE(args->common.volume_options.include_deleted);
    CHECK_FALSE(args->common.volume_options.recover_errors);
  }

  SECTION("every option, before the target") {
    TestIo::Argv argv{L"--directory", L"--deleted", L"--recover", L"disk.img"};
    const auto args =
        ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());

    REQUIRE(args.has_value());
    CHECK(args->target == L"disk.img");
    CHECK(args->directory);
    CHECK(args->common.volume_options.include_deleted);
    CHECK(args->common.volume_options.recover_errors);
  }

  SECTION("the short form of --directory, after the target") {
    TestIo::Argv argv{L"disk.img", L"-d"};
    const auto args =
        ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());

    REQUIRE(args.has_value());
    CHECK(args->directory);
  }

  SECTION("a --log option") {
    TestIo::Argv argv{L"--log=console:error", L"disk.img"};

    CHECK(
        ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data()).has_value());
  }
}

TEST_CASE("NtfsAttr asks for help without a target", "[ntfsattr][arguments]") {
  for (const std::wstring_view option : {L"--help", L"-h"}) {
    TestIo::Argv argv{option};
    const auto args =
        ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());

    REQUIRE(args.has_value());
    CHECK(args->common.help);
  }
}

TEST_CASE("NtfsAttr refuses a bad command line", "[ntfsattr][arguments]") {
  SECTION("no argument") {
    TestIo::Argv argv{};

    CHECK_FALSE(
        ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data()).has_value());
  }

  SECTION("only options") {
    TestIo::Argv argv{L"--directory", L"--deleted"};

    CHECK_FALSE(
        ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data()).has_value());
  }

  SECTION("two targets") {
    TestIo::Argv argv{L"disk.img", L"other.img"};

    CHECK_FALSE(
        ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data()).has_value());
  }

  SECTION("a malformed --log") {
    TestIo::Argv argv{L"--log=bogus", L"disk.img"};

    CHECK_FALSE(
        ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data()).has_value());
  }
}

TEST_CASE("NtfsAttr dumps the file named in an image target",
          "[ntfsattr][run]") {
  const TestIo::TempImage image(DumpTreeImage());
  const std::wstring target = image.path.wstring() + L"/Docs/report.txt";
  TestIo::Argv argv{target};
  const auto args = ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsAttr::DumpTarget(*args, out, err);
      });
  const DumpResult expected = DumpPath(DumpTreeImage(), report_path);

  CHECK(run.exit_code == 0);
  CHECK(run.err.empty());
  CHECK(run.out == TestIo::Narrow(expected.dump));
}

TEST_CASE("NtfsAttr --directory dumps the folder that holds the target",
          "[ntfsattr][run]") {
  const TestIo::TempImage image(DumpTreeImage());
  const std::wstring target = image.path.wstring() + L"/Docs/report.txt";
  TestIo::Argv argv{L"--directory", target};
  const auto args = ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsAttr::DumpTarget(*args, out, err);
      });
  const DumpResult expected = DumpPath(DumpTreeImage(), L"Docs");

  CHECK(run.exit_code == 0);
  CHECK(run.err.empty());
  CHECK(run.out == TestIo::Narrow(expected.dump));
}

TEST_CASE("NtfsAttr dumps the root of an image target with no path",
          "[ntfsattr][run]") {
  const TestIo::TempImage image(DumpTreeImage());
  TestIo::Argv argv{image.path.wstring()};
  const auto args = ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsAttr::DumpTarget(*args, out, err);
      });
  const DumpResult expected = DumpPath(DumpTreeImage(), L"");

  CHECK(run.exit_code == 0);
  CHECK(run.out == TestIo::Narrow(expected.dump));
  CHECK_FALSE(run.out.empty());
}

TEST_CASE("NtfsAttr reports a missing file on stderr", "[ntfsattr][run]") {
  const TestIo::TempImage image(DumpTreeImage());
  const std::wstring target = image.path.wstring() + L"/Docs/nope.txt";
  TestIo::Argv argv{target};
  const auto args = ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsAttr::DumpTarget(*args, out, err);
      });

  CHECK(run.exit_code == ExampleNtfsAttr::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err == "File not found\n");
}

TEST_CASE("NtfsAttr --deleted finds a deleted file", "[ntfsattr][run]") {
  const TestIo::TempImage image(DumpTreeImage());
  const std::wstring target = image.path.wstring() + L"/Docs/old.tmp";
  TestIo::Argv hidden{target};
  TestIo::Argv shown{L"--deleted", target};
  const auto hidden_args =
      ExampleNtfsAttr::ParseArguments(hidden.Count(), hidden.Data());
  const auto shown_args =
      ExampleNtfsAttr::ParseArguments(shown.Count(), shown.Data());
  REQUIRE(hidden_args.has_value());
  REQUIRE(shown_args.has_value());

  const TestIo::Captured without =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsAttr::DumpTarget(*hidden_args, out, err);
      });
  const TestIo::Captured with =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsAttr::DumpTarget(*shown_args, out, err);
      });

  CHECK(without.exit_code == ExampleNtfsAttr::failure_exit_code);
  CHECK(with.exit_code == 0);
  CHECK_FALSE(with.out.empty());
}

#ifndef _WIN32

TEST_CASE("NtfsAttr reports a target that is not NTFS on stderr",
          "[ntfsattr][run]") {
  const TestIo::TempImage image(std::vector<BYTE>(64 * 1024, 0));
  TestIo::Argv argv{image.path.wstring()};
  const auto args = ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsAttr::DumpTarget(*args, out, err);
      });

  CHECK(run.exit_code == ExampleNtfsAttr::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err.starts_with("Cannot get NTFS BPB from boot sector of "));
}

TEST_CASE("NtfsAttr reports a target that names nothing on stderr",
          "[ntfsattr][run]") {
  TestIo::Argv argv{L"/nonexistent/disk.img/Docs"};
  const auto args = ExampleNtfsAttr::ParseArguments(argv.Count(), argv.Data());
  REQUIRE(args.has_value());

  const TestIo::Captured run =
      TestIo::Capture([&](std::FILE* out, std::FILE* err) {
        return ExampleNtfsAttr::DumpTarget(*args, out, err);
      });

  CHECK(run.exit_code == ExampleNtfsAttr::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err ==
        "Cannot find a device or image file in /nonexistent/disk.img/Docs\n");
}

#endif  // !_WIN32

TEST_CASE("NtfsAttr executable prints usage for --help",
          "[ntfsattr][executable]") {
  const std::filesystem::path exe = NTFS_ATTR_EXE;
  REQUIRE(std::filesystem::exists(exe));
  const TestSupport::SampleRun run = TestSupport::RunSample(exe, {L"--help"});

  CHECK(run.exit_code == 0);
  CHECK(run.out == ExampleNtfsAttr::UsageText());
  CHECK(run.err.empty());
}

TEST_CASE("NtfsAttr executable prints usage on stderr for a bad command line",
          "[ntfsattr][executable]") {
  const std::filesystem::path exe = NTFS_ATTR_EXE;
  REQUIRE(std::filesystem::exists(exe));
  const TestSupport::SampleRun run = TestSupport::RunSample(exe, {});

  CHECK(run.exit_code == ExampleNtfsAttr::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err == ExampleNtfsAttr::UsageText());
}

TEST_CASE("NtfsAttr executable dumps a file of an image on stdout",
          "[ntfsattr][executable]") {
  const std::filesystem::path exe = NTFS_ATTR_EXE;
  REQUIRE(std::filesystem::exists(exe));
  const TestIo::TempImage image(DumpTreeImage());
  const TestSupport::SampleRun run = TestSupport::RunSample(
      exe, {L"--recover", image.path.wstring() + L"/Docs/report.txt"});
  const DumpResult expected = DumpPath(DumpTreeImage(), report_path);

  CHECK(run.exit_code == 0);
  CHECK(run.err.empty());
  CHECK(run.out == TestIo::Narrow(expected.dump));
}

TEST_CASE("NtfsAttr executable reports a missing file with a failure code",
          "[ntfsattr][executable]") {
  const std::filesystem::path exe = NTFS_ATTR_EXE;
  REQUIRE(std::filesystem::exists(exe));
  const TestIo::TempImage image(DumpTreeImage());
  const TestSupport::SampleRun run =
      TestSupport::RunSample(exe, {image.path.wstring() + L"/Docs/nope.txt"});

  CHECK(run.exit_code == ExampleNtfsAttr::failure_exit_code);
  CHECK(run.out.empty());
  CHECK(run.err.ends_with("File not found\n"));
}
