#include <ntfs-browser/win-types.h>

#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>

#include "fake-ntfs-image.h"
#include "usn-test-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Usn = NTFS_BROWSER_NAMESPACE::Usn;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::UsnSupport;
using Tests::FakeUsnOptions;
using Tests::FakeUsnRecord;

namespace {

// One terabyte of sparse head: the part of $J that NTFS purged on a volume
// that has seen a lot of change.
constexpr ULONGLONG one_terabyte = ULONGLONG{1} << 40U;

std::vector<FakeUsnRecord> SomeRecords() {
  std::vector<FakeUsnRecord> records;
  for (size_t i = 0; i < 50; ++i) {
    records.push_back(
        {.name = L"file" + std::to_wstring(i), .file_record = 100 + i});
  }
  return records;
}

// Reads the journal of an image, and says how many reads it took.
template <Cache::Strategy S>
struct Outcome {
  size_t reads{0};
  Usn::Journal journal;
};

template <Cache::Strategy S>
Outcome<S> ReadJournalOf(const FakeUsnOptions& options) {
  const auto opened = Support::OpenVolume<S>(Support::JournalImage(options));
  opened.counter->Reset();
  const Usn::Journal* journal = opened.volume->GetUsnJournal();
  REQUIRE(journal != nullptr);
  return {.reads = opened.counter->ReadCount(), .journal = *journal};
}

template <Cache::Strategy S>
void CheckASparseHeadCostsNoRead() {
  const Outcome<S> without = ReadJournalOf<S>({.records = SomeRecords()});
  const Outcome<S> with = ReadJournalOf<S>(
      {.records = SomeRecords(), .sparse_head_bytes = one_terabyte});

  REQUIRE(with.journal.records.size() == 50);
  CHECK(with.reads == without.reads);
  CHECK(with.journal.scanned_bytes == without.journal.scanned_bytes);
  CHECK(with.journal.scanned_bytes > 0);
  CHECK(with.journal.records.front().usn ==
        static_cast<LONGLONG>(one_terabyte));
  CHECK(with.journal.records.back().file_record == 149);
}

template <Cache::Strategy S>
void CheckAHoleInTheMiddleSplitsTheScan() {
  // The first stretch ends in a cut record, then a hole of 1 MiB, then more
  // records: the cut one is damage, and the scan starts clean after the hole.
  std::vector<FakeUsnRecord> first = SomeRecords();
  FakeUsnOptions options{.records = first};
  options.more_stretches.push_back(
      {.hole_bytes = 1024 * 1024,
       .records = {{.name = L"after-hole-1", .file_record = 900},
                   {.name = L"after-hole-2", .file_record = 901}}});

  const Outcome<S> outcome = ReadJournalOf<S>(options);

  REQUIRE(outcome.journal.records.size() == 52);
  CHECK(outcome.journal.records.at(50).file_record == 900);
  CHECK(outcome.journal.records.at(51).file_record == 901);
  CHECK(outcome.journal.records.at(50).usn >=
        outcome.journal.records.at(49).usn + 1024 * 1024);
  CHECK(outcome.journal.damaged_records == 0);
}

template <Cache::Strategy S>
void CheckARecordCutAtAHoleIsDamageAndNotCarried() {
  // The last record of the first stretch declares a length that runs past the
  // stretch's end, as a record whose end was purged would. The bytes after the
  // hole are not its end: the scan counts damage, and starts clean after it.
  const std::vector<FakeUsnRecord> records = SomeRecords();
  size_t last_start = 0;
  size_t offset = 0;
  for (const FakeUsnRecord& record : records) {
    last_start = offset;
    offset += Tests::EncodeUsnRecord(record, offset).size();
  }

  FakeUsnOptions options{.records = records};
  options.more_stretches.push_back(
      {.hole_bytes = 4096,
       .records = {{.name = L"after-hole", .file_record = 900}}});
  constexpr DWORD forged_length = 4000;
  std::vector<BYTE> forged(sizeof(forged_length), 0);
  std::memcpy(forged.data(), &forged_length, sizeof(forged_length));
  options.patches.push_back({.stream_offset = last_start, .bytes = forged});

  const Outcome<S> outcome = ReadJournalOf<S>(options);

  CHECK(outcome.journal.damaged_records >= 1);
  REQUIRE_FALSE(outcome.journal.records.empty());
  CHECK(outcome.journal.records.back().file_record == 900);
  CHECK(outcome.journal.records.size() == 50);
}

}  // namespace

TEST_CASE("a sparse head of a terabyte costs no read", "[usn][sparse]") {
  CheckASparseHeadCostsNoRead<Cache::Strategy::NoCache>();
  CheckASparseHeadCostsNoRead<Cache::Strategy::FullCache>();
}

TEST_CASE("a hole in the middle of the journal splits the scan",
          "[usn][sparse]") {
  CheckAHoleInTheMiddleSplitsTheScan<Cache::Strategy::NoCache>();
  CheckAHoleInTheMiddleSplitsTheScan<Cache::Strategy::FullCache>();
}

TEST_CASE(
    "a record cut at a hole is damage and is not finished by what follows",
    "[usn][sparse]") {
  CheckARecordCutAtAHoleIsDamageAndNotCarried<Cache::Strategy::NoCache>();
  CheckARecordCutAtAHoleIsDamageAndNotCarried<Cache::Strategy::FullCache>();
}
