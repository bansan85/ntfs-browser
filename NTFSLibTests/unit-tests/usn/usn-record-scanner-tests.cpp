#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>

#include "fake-ntfs-image.h"
#include "usn-test-support.h"
#include "usn/usn-record-scanner.h"

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace UsnSupport = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
using NTFS_BROWSER_NAMESPACE::Usn::RecordScanner;
using Tests::FakeUsnRecord;

namespace {

// Offsets of the fields a test forges, in a V2 record.
constexpr size_t v2_length_offset = 0;
constexpr size_t v2_major_offset = 4;
constexpr size_t v2_name_length_offset = 56;
constexpr size_t v2_name_offset_offset = 58;
constexpr size_t v2_name_offset = 60;

FakeUsnRecord Named(const std::wstring& name) {
  return FakeUsnRecord{.name = name, .file_record = 17, .file_sequence = 3};
}

}  // namespace

TEST_CASE("a V2 record is decoded field by field", "[usn][scanner]") {
  const FakeUsnRecord fake{.name = L"report.txt",
                           .file_record = 0x123456,
                           .file_sequence = 7,
                           .parent_record = 16,
                           .parent_sequence = 1,
                           .time = 0x01D87F3C11223344ULL,
                           .reason = 0x80000200,
                           .file_attributes = 0x21};
  const std::vector<BYTE> bytes = Tests::EncodeUsnRecord(fake, 4096);

  const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes, 4096);

  REQUIRE(result.records.size() == 1);
  const auto& record = result.records.front();
  CHECK(record.file_record == 0x123456);
  CHECK(record.file_sequence == 7);
  CHECK(record.parent_record == 16);
  CHECK(record.parent_sequence == 1);
  CHECK(record.usn == 4096);
  CHECK(record.reason == 0x80000200);
  CHECK(record.file_attributes == 0x21);
  CHECK(record.name == L"report.txt");
  CHECK(record.major_version == 2);
  CHECK(record.file_reference_high == 0);
  CHECK(record.time.dwLowDateTime == 0x11223344);
  CHECK(record.time.dwHighDateTime == 0x01D87F3C);
  CHECK(result.damaged == 0);
  CHECK(result.skipped == 0);
}

TEST_CASE("a V3 record keeps the high half of its references",
          "[usn][scanner]") {
  FakeUsnRecord fake = Named(L"wide.bin");
  fake.major_version = 3;
  fake.file_reference_high = 0x1111;
  fake.parent_reference_high = 0x2222;
  const std::vector<BYTE> bytes = Tests::EncodeUsnRecord(fake, 0);

  const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);

  REQUIRE(result.records.size() == 1);
  CHECK(result.records.front().major_version == 3);
  CHECK(result.records.front().file_reference_high == 0x1111);
  CHECK(result.records.front().parent_reference_high == 0x2222);
  CHECK(result.records.front().name == L"wide.bin");
  CHECK(result.records.front().file_record == 17);
}

TEST_CASE("a V4 record is counted and not kept", "[usn][scanner]") {
  FakeUsnRecord range = Named(L"");
  range.major_version = 4;
  std::vector<BYTE> bytes = Tests::EncodeUsnRecord(range, 0);
  const std::vector<BYTE> next =
      Tests::EncodeUsnRecord(Named(L"after.txt"), bytes.size());
  bytes.insert(bytes.end(), next.begin(), next.end());

  const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);

  CHECK(result.skipped == 1);
  CHECK(result.damaged == 0);
  REQUIRE(result.records.size() == 1);
  CHECK(result.records.front().name == L"after.txt");
}

TEST_CASE("zero padding between records is not damage", "[usn][scanner]") {
  std::vector<BYTE> bytes = Tests::EncodeUsnRecord(Named(L"a"), 0);
  bytes.resize(bytes.size() + 4096, 0);
  const std::vector<BYTE> second =
      Tests::EncodeUsnRecord(Named(L"b"), bytes.size());
  bytes.insert(bytes.end(), second.begin(), second.end());

  const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);

  REQUIRE(result.records.size() == 2);
  CHECK(result.records.at(0).name == L"a");
  CHECK(result.records.at(1).name == L"b");
  CHECK(result.damaged == 0);
}

TEST_CASE("a record length that cannot be a record is damage",
          "[usn][scanner]") {
  const std::vector<BYTE> good = Tests::EncodeUsnRecord(Named(L"file.txt"), 0);

  SECTION("not a multiple of 8") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<DWORD>(bytes, v2_length_offset, 62);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.empty());
    CHECK(result.damaged >= 1);
  }

  SECTION("below the fixed part of the version") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<DWORD>(bytes, v2_length_offset, 56);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.empty());
    CHECK(result.damaged >= 1);
  }

  SECTION("above the longest record of a named version") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<DWORD>(bytes, v2_length_offset, 8192);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.empty());
    CHECK(result.damaged >= 1);
  }

  SECTION("an unknown major version") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<WORD>(bytes, v2_major_offset, 9);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.empty());
    CHECK(result.damaged >= 1);
  }
}

TEST_CASE("a name that is not well-formed makes the record damage",
          "[usn][scanner]") {
  const std::vector<BYTE> good = Tests::EncodeUsnRecord(Named(L"file.txt"), 0);

  SECTION("an odd name length") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<WORD>(bytes, v2_name_length_offset, 15);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.empty());
    CHECK(result.damaged >= 1);
  }

  SECTION("a name that leaves the record") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<WORD>(bytes, v2_name_length_offset, 2000);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.empty());
    CHECK(result.damaged >= 1);
  }

  SECTION("a name that starts inside the fixed part") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<WORD>(bytes, v2_name_offset_offset, 20);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.empty());
    CHECK(result.damaged >= 1);
  }

  SECTION("a lone high surrogate") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<WORD>(bytes, v2_name_offset, 0xD800);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.empty());
    CHECK(result.damaged >= 1);
  }

  SECTION("a lone low surrogate") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<WORD>(bytes, v2_name_offset, 0xDC00);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.empty());
    CHECK(result.damaged >= 1);
  }

  SECTION("a surrogate pair is valid") {
    std::vector<BYTE> bytes = good;
    UsnSupport::PutAt<WORD>(bytes, v2_name_offset, 0xD83D);
    UsnSupport::PutAt<WORD>(bytes, v2_name_offset + 2, 0xDE00);
    const UsnSupport::ScanResult result = UsnSupport::ScanBytes(bytes);
    CHECK(result.records.size() == 1);
    CHECK(result.damaged == 0);
  }
}

TEST_CASE("a record split across two Feed calls is finished by the second",
          "[usn][scanner]") {
  std::vector<BYTE> bytes = Tests::EncodeUsnRecord(Named(L"split.txt"), 0);
  const std::vector<BYTE> second =
      Tests::EncodeUsnRecord(Named(L"next.txt"), bytes.size());
  bytes.insert(bytes.end(), second.begin(), second.end());

  for (size_t cut = 1; cut < bytes.size(); ++cut) {
    RecordScanner scanner;
    std::vector<NTFS_BROWSER_NAMESPACE::Usn::Record> records;
    scanner.Reset();
    scanner.Feed(std::span<const BYTE>(bytes).first(cut), 0, records);
    scanner.Feed(std::span<const BYTE>(bytes).subspan(cut), cut, records);
    scanner.Finish(records);
    INFO("cut at " << cut);
    REQUIRE(records.size() == 2);
    CHECK(records.at(0).name == L"split.txt");
    CHECK(records.at(1).name == L"next.txt");
    CHECK(scanner.DamagedRecords() == 0);
  }
}

TEST_CASE("a record cut by the end of its range is one damaged record",
          "[usn][scanner]") {
  const std::vector<BYTE> bytes =
      Tests::EncodeUsnRecord(Named(L"long-file-name.txt"), 0);

  for (size_t cut = 1; cut < bytes.size(); ++cut) {
    const UsnSupport::ScanResult result =
        UsnSupport::ScanBytes(std::span<const BYTE>(bytes).first(cut));
    INFO("cut at " << cut);
    CHECK(result.records.empty());
    CHECK(result.damaged == 1);
  }

  const UsnSupport::ScanResult whole = UsnSupport::ScanBytes(bytes);
  CHECK(whole.records.size() == 1);
  CHECK(whole.damaged == 0);
}
