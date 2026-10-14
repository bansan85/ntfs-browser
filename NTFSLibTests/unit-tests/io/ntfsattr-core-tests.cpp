#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfsattr-core.h"
#include "sample-dump-support.h"

namespace {

using FullCacheVolume =
    NtfsBrowser::NtfsVolume<NtfsBrowser::Cache::Strategy::FullCache>;
using NtfsAttrDump::DumpResult;
using NtfsBrowserTests::DumpTreeImage;

// A volume over image, opened the way the dialog opens it.
std::unique_ptr<FullCacheVolume> OpenVolume(std::vector<BYTE> image) {
  return std::make_unique<FullCacheVolume>(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(std::move(image)));
}

// Dumps path from a fresh volume over image.
DumpResult DumpPath(std::vector<BYTE> image, std::wstring_view path) {
  const std::unique_ptr<FullCacheVolume> volume = OpenVolume(std::move(image));
  return NtfsAttrDump::DumpAttributes(*volume, path);
}

// The in-use file of the dump tree, with resident data.
constexpr std::wstring_view report_path = L"C:\\Docs\\report.txt";

// Position just past the heading line name in dump, searched from from. npos
// when dump has no such heading after from.
std::size_t FindHeading(const std::wstring& dump, std::wstring_view name,
                        std::size_t from) {
  const std::wstring heading =
      std::wstring(L"\r\n") + std::wstring(name) + std::wstring(L"\r\n");
  const std::size_t at = dump.find(heading, from);
  return at == std::wstring::npos ? at : at + heading.size();
}

}  // namespace

TEST_CASE("NtfsAttr target path keeps the picked file, or its folder",
          "[ntfsattr][characterization]") {
  SECTION("a file is dumped as picked") {
    REQUIRE(NtfsAttrDump::TargetPath(report_path, false) == report_path);
  }

  SECTION("a folder is the directory holding the picked file") {
    REQUIRE(NtfsAttrDump::TargetPath(report_path, true) == L"C:\\Docs");
  }

  SECTION("a file in the root gives the drive") {
    REQUIRE(NtfsAttrDump::TargetPath(L"C:\\big.bin", true) == L"C:");
  }

  SECTION("a path with no separator gives nothing in folder mode") {
    REQUIRE(NtfsAttrDump::TargetPath(L"big.bin", true).empty());
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
  const DumpResult result = DumpPath(
      NtfsBrowserTests::BuildFakeNtfsImageWithCorruptRootRecord(), report_path);

  REQUIRE(result.message == L"Cannot read root directory of volume");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports root attributes that do not parse",
          "[ntfsattr][characterization]") {
  const DumpResult result =
      DumpPath(NtfsBrowserTests::BuildFakeNtfsImageWithAttrOffsetOutOfBounds(),
               report_path);

  REQUIRE(result.message == L"Cannot parse attributes");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports a path entry that is not in its directory",
          "[ntfsattr][characterization]") {
  SECTION("in the last component") {
    const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\nope.txt");

    REQUIRE(result.message == L"File not found\n");
    REQUIRE(result.dump.empty());
  }

  SECTION("in a directory on the path") {
    const DumpResult result =
        DumpPath(DumpTreeImage(), L"C:\\nope\\report.txt");

    REQUIRE(result.message == L"File not found\n");
    REQUIRE(result.dump.empty());
  }
}

TEST_CASE("NtfsAttr reports a directory on the path that does not read",
          "[ntfsattr][characterization]") {
  const DumpResult result =
      DumpPath(DumpTreeImage(), L"C:\\Docs\\Broken\\report.txt");

  // The sample words this message as "root directory" here too.
  REQUIRE(result.message == L"Cannot read root directory of volume");
  REQUIRE(result.dump.empty());
}

TEST_CASE(
    "NtfsAttr reports a directory on the path whose attributes do not "
    "parse",
    "[ntfsattr][characterization]") {
  // OldDir is freed, and a dump does not read freed records.
  const DumpResult result =
      DumpPath(DumpTreeImage(), L"C:\\Docs\\OldDir\\draft.doc");

  REQUIRE(result.message == L"Cannot parse directory attributes");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports a final entry that does not read",
          "[ntfsattr][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\Docs\\Broken");

  REQUIRE(result.message == L"Cannot read file");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports a final file whose attributes do not parse",
          "[ntfsattr][characterization]") {
  // old.tmp is freed, so its attributes stay unread.
  const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\Docs\\old.tmp");

  REQUIRE(result.message == L"Cannot parse file attributes");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr dumps every attribute of a file, in record order",
          "[ntfsattr][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), report_path);

  REQUIRE(result.message.empty());

  // Each heading is the attribute name, and each dump row ends in CRLF.
  const std::size_t standard_info =
      FindHeading(result.dump, L"STANDARD_INFORMATION", 0);
  REQUIRE(standard_info != std::wstring::npos);
  const std::size_t file_name = FindHeading(result.dump, L"FILE_NAME", 0);
  REQUIRE(file_name > standard_info);
  const std::size_t data = FindHeading(result.dump, L"DATA", file_name);
  REQUIRE(data != std::wstring::npos);
  REQUIRE(result.dump.starts_with(L"\r\nSTANDARD_INFORMATION\r\n"));

  // The resident $DATA holds dump_report_content and pads to 48 bytes. Its
  // last row is bytes 32 to 47: the content's tail, then five zeros.
  const std::wstring last_row =
      L"70 20 74 65 73 74 01 7F - E9 0D 0A 00 00 00 00 00   "
      L"p test.\u007F\u00E9" +
      std::wstring(7, L'.');
  REQUIRE(result.dump.ends_with(last_row + L"\r\n"));
}

TEST_CASE("NtfsAttr dumps a folder's attributes, index included",
          "[ntfsattr][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\Docs");

  REQUIRE(result.message.empty());
  REQUIRE(result.dump.starts_with(L"\r\nSTANDARD_INFORMATION\r\n"));
  REQUIRE(FindHeading(result.dump, L"INDEX_ROOT", 0) != std::wstring::npos);
}

TEST_CASE("NtfsAttr dumps the root directory for a bare drive path",
          "[ntfsattr][characterization]") {
  // The root lists itself as ".", so the bare drive resolves to the root.
  const DumpResult result = DumpPath(DumpTreeImage(), L"C:");

  REQUIRE(result.message.empty());
  REQUIRE(result.dump.starts_with(L"\r\nSTANDARD_INFORMATION\r\n"));
  REQUIRE(FindHeading(result.dump, L"INDEX_ROOT", 0) != std::wstring::npos);
}

TEST_CASE("NtfsAttr folder mode dumps the folder holding the picked file",
          "[ntfsattr][characterization]") {
  const auto volume = OpenVolume(DumpTreeImage());
  const DumpResult by_folder = NtfsAttrDump::DumpAttributes(
      *volume, NtfsAttrDump::TargetPath(report_path, true));
  const DumpResult by_folder_name =
      NtfsAttrDump::DumpAttributes(*volume, L"C:\\Docs");

  REQUIRE(by_folder.message.empty());
  REQUIRE(by_folder.dump == by_folder_name.dump);
}
