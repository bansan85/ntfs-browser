#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfsattr-core.h"
#include "sample-dump-support.h"

namespace {

using FullCacheVolume = NTFS_BROWSER_NAMESPACE::NtfsVolume<
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache>;
using NTFS_BROWSER_NAMESPACE::Tests::DumpTreeImage;
using NtfsAttrDump::DumpResult;

// A volume over image, opened the way the dialog opens it.
std::unique_ptr<FullCacheVolume>
    OpenVolume(std::vector<BYTE> image,
               NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  return std::make_unique<FullCacheVolume>(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          std::move(image)),
      options);
}

// Dumps path from a fresh volume over image.
DumpResult DumpPath(std::vector<BYTE> image, std::wstring_view path,
                    NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  const std::unique_ptr<FullCacheVolume> volume =
      OpenVolume(std::move(image), options);
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
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithCorruptRootRecord(),
      report_path);

  REQUIRE(result.message == L"Cannot read root directory of volume");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports root attributes that do not parse",
          "[ntfsattr][characterization]") {
  const DumpResult result =
      DumpPath(NTFS_BROWSER_NAMESPACE::Tests::
                   BuildFakeNtfsImageWithAttrOffsetOutOfBounds(),
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
  SECTION("strict hides the damaged directory") {
    const DumpResult result =
        DumpPath(DumpTreeImage(), L"C:\\Docs\\Broken\\report.txt");

    REQUIRE(result.message == L"File not found\n");
    REQUIRE(result.dump.empty());
  }

  SECTION("recovering reads the directory that does not read") {
    // The sample words this message as "root directory" here too.
    const DumpResult result =
        DumpPath(DumpTreeImage(), L"C:\\Docs\\Broken\\report.txt",
                 {.recover_errors = true});

    REQUIRE(result.message == L"Cannot read root directory of volume");
    REQUIRE(result.dump.empty());
  }
}

TEST_CASE("NtfsAttr reports a freed directory on the path as not found",
          "[ntfsattr][characterization]") {
  // OldDir is freed, and a default lookup hides freed names.
  const DumpResult result =
      DumpPath(DumpTreeImage(), L"C:\\Docs\\OldDir\\draft.doc");

  REQUIRE(result.message == L"File not found\n");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsAttr reports a final entry that does not read",
          "[ntfsattr][characterization]") {
  SECTION("strict hides the damaged entry") {
    const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\Docs\\Broken");

    REQUIRE(result.message == L"File not found\n");
    REQUIRE(result.dump.empty());
  }

  SECTION("recovering reads the entry that does not read") {
    const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\Docs\\Broken",
                                       {.recover_errors = true});

    REQUIRE(result.message == L"Cannot read file");
    REQUIRE(result.dump.empty());
  }
}

TEST_CASE("NtfsAttr reports a freed final file as not found",
          "[ntfsattr][characterization]") {
  // old.tmp is freed, so a default lookup hides it.
  const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\Docs\\old.tmp");

  REQUIRE(result.message == L"File not found\n");
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

TEST_CASE("NtfsAttr dump of zero bytes has no rows",
          "[ntfsattr][characterization]") {
  const std::vector<BYTE> bytes(16, 0xAA);
  std::wstring lines;

  NtfsAttrDump::AppendData(lines, bytes.data(), 0);

  REQUIRE(lines.empty());
}

TEST_CASE("NtfsAttr dump rows split at 16 bytes, last row padded with FF",
          "[ntfsattr][characterization]") {
  std::vector<BYTE> bytes(17, 0x41);
  bytes[16] = 0x42;
  std::wstring lines;

  NtfsAttrDump::AppendData(lines, bytes.data(), 17);

  // The padding bytes are 0xFF, which the text column shows as U+00FF.
  REQUIRE(lines ==
          L"41 41 41 41 41 41 41 41 - 41 41 41 41 41 41 41 41   "
          L"AAAAAAAAAAAAAAAA\r\n" +
              std::wstring(
                  L"42 FF FF FF FF FF FF FF - FF FF FF FF FF FF FF FF   B") +
              std::wstring(15, L'\u00FF') + L"\r\n");
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
    REQUIRE(NtfsAttrDump::AttrTypeName(type) == name);
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
    REQUIRE(NtfsAttrDump::AttrTypeName(static_cast<Type>(raw)) == label);
  }
}
