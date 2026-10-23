#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "usn-test-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Usn = NTFS_BROWSER_NAMESPACE::Usn;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeUsnRecord;

namespace {

// Records of the small journals below.
constexpr size_t small_count = 20;

std::vector<FakeUsnRecord> SmallJournal() {
  std::vector<FakeUsnRecord> records;
  for (size_t i = 0; i < small_count; ++i) {
    records.push_back(
        {.name = L"file" + std::to_wstring(1000 + i), .file_record = 100 + i});
  }
  return records;
}

VolumeOptions WithCap(std::uint64_t cap) {
  return VolumeOptions{
      .include_deleted = true, .recover_errors = true, .max_usn_records = cap};
}

template <Cache::Strategy S>
void CheckTheNewestRecordsAreKept() {
  const auto opened = Support::OpenVolume<S>(
      Support::JournalImage({.records = SmallJournal()}), WithCap(5));

  const Usn::Journal* journal = opened.volume->GetUsnJournal();

  REQUIRE(journal != nullptr);
  REQUIRE(journal->records.size() == 5);
  CHECK(journal->truncated);
  CHECK(journal->records.front().file_record == 100 + small_count - 5);
  CHECK(journal->records.back().file_record == 100 + small_count - 1);
  for (size_t i = 1; i < journal->records.size(); ++i) {
    CHECK(journal->records.at(i).usn > journal->records.at(i - 1).usn);
  }
}

template <Cache::Strategy S>
void CheckACapOfZeroKeepsNone() {
  const auto opened = Support::OpenVolume<S>(
      Support::JournalImage({.records = SmallJournal()}), WithCap(0));

  const Usn::Journal* journal = opened.volume->GetUsnJournal();

  REQUIRE(journal != nullptr);
  CHECK(journal->records.empty());
  CHECK(journal->truncated);
  CHECK(journal->scanned_bytes > 0);
}

template <Cache::Strategy S>
void CheckNoLimitKeepsAll() {
  const auto opened =
      Support::OpenVolume<S>(Support::JournalImage({.records = SmallJournal()}),
                             WithCap(Usn::unlimited_records));

  const Usn::Journal* journal = opened.volume->GetUsnJournal();

  REQUIRE(journal != nullptr);
  CHECK(journal->records.size() == small_count);
  CHECK_FALSE(journal->truncated);
}

template <Cache::Strategy S>
void CheckACapAboveTheCountIsNotTruncated() {
  const auto opened = Support::OpenVolume<S>(
      Support::JournalImage({.records = SmallJournal()}), WithCap(small_count));

  const Usn::Journal* journal = opened.volume->GetUsnJournal();

  REQUIRE(journal != nullptr);
  CHECK(journal->records.size() == small_count);
  CHECK_FALSE(journal->truncated);
}

}  // namespace

TEST_CASE("a limit keeps the newest records and says it cut", "[usn][cap]") {
  CheckTheNewestRecordsAreKept<Cache::Strategy::NoCache>();
  CheckTheNewestRecordsAreKept<Cache::Strategy::FullCache>();
}

TEST_CASE("a limit of zero keeps none but still reads the journal",
          "[usn][cap]") {
  CheckACapOfZeroKeepsNone<Cache::Strategy::NoCache>();
  CheckACapOfZeroKeepsNone<Cache::Strategy::FullCache>();
}

TEST_CASE("no limit keeps every record", "[usn][cap]") {
  CheckNoLimitKeepsAll<Cache::Strategy::NoCache>();
  CheckNoLimitKeepsAll<Cache::Strategy::FullCache>();
}

TEST_CASE("a limit equal to the count is not a truncation", "[usn][cap]") {
  CheckACapAboveTheCountIsNotTruncated<Cache::Strategy::NoCache>();
  CheckACapAboveTheCountIsNotTruncated<Cache::Strategy::FullCache>();
}

TEST_CASE("the default limit holds a journal of 100 MiB whole",
          "[usn][cap][slow]") {
  // The smallest V2 record is 64 bytes: a header of 60 and a one-character
  // name, rounded up to 8. A journal of 100 MiB cannot hold more of them than
  // this.
  constexpr size_t journal_bytes = size_t{100} * 1024 * 1024;
  constexpr size_t record_bytes = 64;
  constexpr size_t count = journal_bytes / record_bytes;
  static_assert(count == Usn::default_max_records);

  std::vector<BYTE> stream;
  stream.reserve(journal_bytes);
  for (size_t i = 0; i < count; ++i) {
    const std::vector<BYTE> record = Tests::EncodeUsnRecord(
        FakeUsnRecord{.name = L"a", .file_record = i}, stream.size());
    REQUIRE(record.size() == record_bytes);
    stream.insert(stream.end(), record.begin(), record.end());
  }
  REQUIRE(stream.size() == journal_bytes);

  VolumeOptions options{.include_deleted = true, .recover_errors = true};
  const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(
      Support::JournalImage({.raw_stream = std::move(stream)}), options);

  const Usn::Journal* journal = opened.volume->GetUsnJournal();

  REQUIRE(journal != nullptr);
  CHECK(journal->records.size() == count);
  CHECK_FALSE(journal->truncated);
  CHECK(journal->damaged_records == 0);
  CHECK(journal->records.back().usn ==
        static_cast<LONGLONG>((count - 1) * record_bytes));

  // The memory the result holds stays under 330 MiB: the bound the default
  // promises (SC-006).
  size_t held = journal->records.size() * sizeof(Usn::Record);
  for (const Usn::Record& record : journal->records) {
    held += record.name.capacity() * sizeof(wchar_t);
  }
  CHECK(held < size_t{330} * 1024 * 1024);
}
