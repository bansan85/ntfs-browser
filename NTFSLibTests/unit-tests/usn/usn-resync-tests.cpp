#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>

#include "alloc-failure-support.h"
#include "catch2/catch_message.hpp"
#include "fake-ntfs-image.h"
#include "usn-test-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Usn = NTFS_BROWSER_NAMESPACE::Usn;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
namespace AllocSupport = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Tests::FakeUsnRecord;

namespace {

// Offset of the major version in a record, and a version no journal has.
constexpr size_t major_version_offset = 4;
constexpr WORD unknown_version = 9;

// Offset of the length in a record.
constexpr size_t length_offset = 0;

// The record number a fixture file i has.
ULONGLONG RecordNumberOf(size_t index) { return 100 + index; }

// Records "file<i>" laid back to back from offset 0, and where each one starts.
struct Journal {
  std::vector<BYTE> bytes;
  std::vector<size_t> starts;
};

// A journal of count records, every name of the same length.
Journal Build(size_t count) {
  Journal journal;
  for (size_t i = 0; i < count; ++i) {
    FakeUsnRecord record{.name = L"file" + std::to_wstring(1000 + i),
                         .file_record = RecordNumberOf(i)};
    journal.starts.push_back(journal.bytes.size());
    const std::vector<BYTE> encoded =
        Tests::EncodeUsnRecord(record, journal.bytes.size());
    journal.bytes.insert(journal.bytes.end(), encoded.begin(), encoded.end());
  }
  return journal;
}

}  // namespace

TEST_CASE("one corrupted record among ten thousand costs one record",
          "[usn][resync]") {
  Journal journal = Build(10000);
  Support::PutAt<WORD>(journal.bytes,
                       journal.starts.at(5000) + major_version_offset,
                       unknown_version);

  const Support::ScanResult result = Support::ScanBytes(journal.bytes);

  CHECK(result.records.size() == 9999);
  CHECK(result.damaged == 1);
  CHECK(result.records.at(4999).file_record == RecordNumberOf(4999));
  CHECK(result.records.at(5000).file_record == RecordNumberOf(5001));
  CHECK(result.records.back().file_record == RecordNumberOf(9999));
}

TEST_CASE("a forged length that no record can have is skipped",
          "[usn][resync]") {
  for (const DWORD forged : {DWORD{0}, DWORD{8}, DWORD{62}, DWORD{8192}}) {
    INFO("length " << forged);
    Journal journal = Build(10);
    Support::PutAt<DWORD>(journal.bytes, journal.starts.at(3) + length_offset,
                          forged);

    const Support::ScanResult result = Support::ScanBytes(journal.bytes);

    REQUIRE(result.records.size() == 9);
    CHECK(result.records.at(2).file_record == RecordNumberOf(2));
    CHECK(result.records.at(3).file_record == RecordNumberOf(4));
    CHECK(result.damaged == 1);
  }
}

TEST_CASE(
    "a plausible length that is too large costs what it swallows, no more",
    "[usn][resync]") {
  Journal journal = Build(40);
  Support::PutAt<DWORD>(journal.bytes, journal.starts.at(3) + length_offset,
                        1024);

  const Support::ScanResult result = Support::ScanBytes(journal.bytes);

  // The first records, and the last one, come back. The scan finds the next
  // record again by its USN, wherever the swallowed bytes end.
  REQUIRE(result.records.size() >= 3);
  CHECK(result.records.at(2).file_record == RecordNumberOf(2));
  CHECK(result.records.back().file_record == RecordNumberOf(39));
  CHECK(result.records.size() < 40);
}

TEST_CASE("a journal cut in the middle of its last record keeps the others",
          "[usn][resync]") {
  const Journal journal = Build(6);
  const std::vector<BYTE> cut(
      journal.bytes.begin(),
      journal.bytes.begin() +
          static_cast<std::ptrdiff_t>(journal.starts.at(5) + 20));

  const Support::ScanResult result = Support::ScanBytes(cut);

  CHECK(result.records.size() == 5);
  CHECK(result.damaged == 1);
}

TEST_CASE("records of every version mix in one journal", "[usn][resync]") {
  std::vector<BYTE> bytes;
  const WORD versions[] = {2, 3, 4, 2, 3};
  for (const WORD version : versions) {
    FakeUsnRecord record{.name = version == 4 ? L"" : L"mixed.txt",
                         .major_version = version,
                         .file_record = ULONGLONG{20} + version};
    const std::vector<BYTE> encoded =
        Tests::EncodeUsnRecord(record, bytes.size());
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
  }

  const Support::ScanResult result = Support::ScanBytes(bytes);

  CHECK(result.records.size() == 4);
  CHECK(result.skipped == 1);
  CHECK(result.damaged == 0);
  CHECK(result.records.at(0).major_version == 2);
  CHECK(result.records.at(1).major_version == 3);
  CHECK(result.records.at(2).major_version == 2);
  CHECK(result.records.at(3).major_version == 3);
}

TEST_CASE("after damage a record is taken only when its USN is its offset",
          "[usn][resync]") {
  std::vector<BYTE> bytes =
      Tests::EncodeUsnRecord(FakeUsnRecord{.name = L"first.txt"}, 0);
  // Twenty-four bytes that are neither a record nor padding.
  bytes.insert(bytes.end(), 24, 0xAA);
  FakeUsnRecord wrong{.name = L"wrong.txt", .file_record = 2};
  wrong.usn = 12345;
  const std::vector<BYTE> wrong_bytes =
      Tests::EncodeUsnRecord(wrong, bytes.size());
  bytes.insert(bytes.end(), wrong_bytes.begin(), wrong_bytes.end());
  const std::vector<BYTE> last =
      Tests::EncodeUsnRecord(FakeUsnRecord{.name = L"last.txt"}, bytes.size());
  bytes.insert(bytes.end(), last.begin(), last.end());

  const Support::ScanResult result = Support::ScanBytes(bytes);

  REQUIRE(result.records.size() == 2);
  CHECK(result.records.at(0).name == L"first.txt");
  CHECK(result.records.at(1).name == L"last.txt");
  CHECK(result.damaged == 1);
}

TEST_CASE("random bytes end in bounded time and never throw", "[usn][resync]") {
  for (const unsigned seed : {1U, 2U, 3U}) {
    std::mt19937 generator(seed);
    std::vector<BYTE> bytes(size_t{1} << 20U);
    for (BYTE& byte : bytes) {
      byte = static_cast<BYTE>(generator());
    }

    CHECK_NOTHROW(Support::ScanBytes(bytes, size_t{4096} * seed));
  }
}

TEST_CASE("bytes that look like records but are not, never throw",
          "[usn][resync]") {
  // A header of every version and length, over zeros and over 0xFF.
  for (const BYTE fill : {BYTE{0x00}, BYTE{0xFF}}) {
    for (WORD version = 0; version < 6; ++version) {
      for (DWORD length = 0; length <= 128; length += 8) {
        std::vector<BYTE> bytes(512, fill);
        Support::PutAt<DWORD>(bytes, 0, length);
        Support::PutAt<WORD>(bytes, 4, version);
        Support::PutAt<WORD>(bytes, 6, 0);
        CHECK_NOTHROW(Support::ScanBytes(bytes));
      }
    }
  }
}

namespace {

template <Cache::Strategy S>
void CheckDamageIsLoggedOncePerVolume() {
  const Journal journal = Build(6);
  Tests::FakeUsnOptions options{.raw_stream = journal.bytes};
  for (const size_t index : {size_t{1}, size_t{3}, size_t{5}}) {
    options.patches.push_back(
        {.stream_offset = journal.starts.at(index) + major_version_offset,
         .bytes = {static_cast<BYTE>(unknown_version), 0x00}});
  }

  const auto sink = std::make_shared<AllocSupport::LevelSink>();
  const AllocSupport::ScopedSink scoped(sink);
  const auto opened = Support::Open<S>(Support::JournalImage(options));

  const Usn::Journal* result = opened.volume->GetUsnJournal();
  REQUIRE(result != nullptr);
  CHECK(result->records.size() == 3);
  CHECK(result->damaged_records == 3);

  constexpr std::string_view message = "damaged stretch";
  CHECK(sink->Count(message) == 1);
  CHECK(sink->LevelOf(message) == Log::Level::Info);

  // Asking again logs nothing more.
  CHECK(opened.volume->GetUsnJournal() == result);
  CHECK(sink->Count(message) == 1);
}

}  // namespace

TEST_CASE("damage in the journal is logged once per volume", "[usn][resync]") {
  CheckDamageIsLoggedOncePerVolume<Cache::Strategy::NoCache>();
  CheckDamageIsLoggedOncePerVolume<Cache::Strategy::FullCache>();
}
