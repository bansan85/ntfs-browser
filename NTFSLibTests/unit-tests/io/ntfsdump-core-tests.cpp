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
#include "ntfsdump-core.h"
#include "sample-dump-support.h"

namespace {

using NoCacheVolume = NTFS_BROWSER_NAMESPACE::NtfsVolume<
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>;
using NTFS_BROWSER_NAMESPACE::Tests::DumpTreeImage;
using NtfsDump::DumpResult;

// A volume over image, opened the way the dialog opens it.
std::unique_ptr<NoCacheVolume>
    OpenVolume(std::vector<BYTE> image,
               NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  return std::make_unique<NoCacheVolume>(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          std::move(image)),
      options);
}

// Dumps path from a fresh volume over image.
DumpResult DumpPath(std::vector<BYTE> image, std::wstring_view path,
                    NTFS_BROWSER_NAMESPACE::VolumeOptions options = {}) {
  const std::unique_ptr<NoCacheVolume> volume =
      OpenVolume(std::move(image), options);
  return NtfsDump::DumpFile(*volume, path);
}

// The in-use file of the dump tree, with resident data.
constexpr std::wstring_view report_path = L"C:\\Docs\\report.txt";

// The non-resident file of the dump tree, past 16 KiB.
constexpr std::wstring_view big_path = L"C:\\big.bin";

// Text column of one dump row. A byte under 0x20 shows as '.'.
std::wstring TextColumn(const std::vector<unsigned>& bytes) {
  std::wstring text;
  for (const unsigned byte : bytes) {
    text += static_cast<wchar_t>(byte < 0x20 ? '.' : byte);
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
  const DumpResult result = DumpPath(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithCorruptRootRecord(),
      report_path);

  REQUIRE(result.message == L"Cannot read root directory of volume");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reports root attributes that do not parse",
          "[ntfsdump][characterization]") {
  const DumpResult result =
      DumpPath(NTFS_BROWSER_NAMESPACE::Tests::
                   BuildFakeNtfsImageWithAttrOffsetOutOfBounds(),
               report_path);

  REQUIRE(result.message == L"Cannot parse attributes");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reports a path entry that is not in its directory",
          "[ntfsdump][characterization]") {
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

TEST_CASE("NtfsDump reports a directory on the path that does not read",
          "[ntfsdump][characterization]") {
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

TEST_CASE("NtfsDump reports a freed directory on the path as not found",
          "[ntfsdump][characterization]") {
  // OldDir is freed, and a default lookup hides freed names.
  const DumpResult result =
      DumpPath(DumpTreeImage(), L"C:\\Docs\\OldDir\\draft.doc");

  REQUIRE(result.message == L"File not found\n");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reports a final entry that does not read",
          "[ntfsdump][characterization]") {
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

TEST_CASE("NtfsDump reports a freed final file as not found",
          "[ntfsdump][characterization]") {
  // old.tmp is freed, so a default lookup hides it.
  const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\Docs\\old.tmp");

  REQUIRE(result.message == L"File not found\n");
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump reports a data run that cannot be read",
          "[ntfsdump][characterization]") {
  // Clusters past the end of the image, so the read fails.
  const DumpResult result =
      DumpPath(NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithDumpTree(
                   NTFS_BROWSER_NAMESPACE::Tests::dump_big_file_size,
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
  const DumpResult result =
      DumpPath(NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithDumpTree(
                   NTFS_BROWSER_NAMESPACE::Tests::dump_big_file_size,
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
  // bytes of the last row show as 0xFF in both columns, a quirk kept for now.
  const std::wstring first_row =
      L"0000    4E 54 46 53 20 64 75 6D - 70 20 74 65 73 74 01 7F   "
      L"NTFS dump test.\u007F";
  std::vector<unsigned> last_bytes{0xE9, 0x0D, 0x0A};
  last_bytes.resize(16, 0xFF);
  const std::wstring last_row =
      L"0010    E9 0D 0A FF FF FF FF FF - FF FF FF FF FF FF FF FF   " +
      TextColumn(last_bytes);
  REQUIRE(result.dump == first_row + L"\r\n" + last_row);
}

TEST_CASE("NtfsDump cuts a file at 16 KiB", "[ntfsdump][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), big_path);

  REQUIRE(result.message.empty());

  // 16384 bytes make 1024 rows. Only the rows before the last one end in CRLF.
  std::size_t crlfs = 0;
  for (std::size_t at = result.dump.find(L"\r\n"); at != std::wstring::npos;
       at = result.dump.find(L"\r\n", at + 2)) {
    ++crlfs;
  }
  REQUIRE(crlfs == 1023);

  // The file's byte k is k & 0xFF, so the last row holds bytes 0xF0 to 0xFF.
  std::vector<unsigned> last_bytes;
  for (unsigned byte = 0xF0; byte <= 0xFF; ++byte) {
    last_bytes.push_back(byte);
  }
  const std::wstring last_row =
      L"3FF0    F0 F1 F2 F3 F4 F5 F6 F7 - F8 F9 FA FB FC FD FE FF   " +
      TextColumn(last_bytes);
  REQUIRE(result.dump.size() >= last_row.size());
  REQUIRE(result.dump.substr(result.dump.size() - last_row.size()) == last_row);
}

TEST_CASE("NtfsDump dumps nothing for a directory, which has no data stream",
          "[ntfsdump][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\Docs");

  REQUIRE(result.message.empty());
  REQUIRE(result.dump.empty());
}

TEST_CASE("NtfsDump dumps nothing for a zero-byte file",
          "[ntfsdump][characterization]") {
  const DumpResult result = DumpPath(DumpTreeImage(), L"C:\\empty.txt");

  REQUIRE(result.message.empty());
  REQUIRE(result.dump.empty());
}
