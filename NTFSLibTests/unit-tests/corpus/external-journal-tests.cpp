#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/usn/types.h>
#include <ntfs-browser/volume-options.h>

#include "corpus-test-support.h"
#include "external-journal-support.h"
#include "external-mft-support.h"
#include "fake-ntfs-image.h"
#include "usn-test-support.h"

namespace {

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Usn = NTFS_BROWSER_NAMESPACE::Usn;
namespace RealJournal = NTFS_BROWSER_NAMESPACE::Tests::RealJournal;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using RealJournal::Fixture;
using RealJournal::JournalSample;
using Tests::DfirNtfsDir;
using Tests::EntryUtcSeconds;
using Tests::EntryUtcTicks;
using Tests::ReadCorpusFile;
using Tests::RequireCorpusFile;
using Tests::UsnSupport::RecoverOptions;

// Bytes of a cluster of the fake volume: the unit of a sparse head.
constexpr ULONGLONG cluster_bytes = Tests::fake_cluster_size;

// Ticks (100 ns) in one microsecond.
constexpr std::uint64_t ticks_per_microsecond = 10;

// Bytes of a mid-record cut: far enough into a record to leave its header
// partly there, and short of the name offset (0x38), so the cut record cannot
// pass for a whole one once the stream is padded with zeros.
constexpr std::uint64_t cut_into_record = 30;

// Bytes by which a damaged range reaches into a record at either end, in the
// tests whose damage does not fall on a record boundary.
constexpr std::uint64_t into_record = 20;

// Records NTFS wrote to dfir_ntfs's journal after fsutil printed its listing:
// those with a USN from the listing's "Next USN" on.
constexpr std::size_t records_after_fsutil = 3;

// Records of the fsutil listing that carry no name: the V4 ones.
constexpr std::size_t fsutil_range_records = 7;

// Records of the fsutil listing: 268 blocks, 261 of them for V2 records and 7
// for V4 ones.
constexpr std::size_t fsutil_record_count = 268;

std::vector<BYTE> BytesOf(const Fixture& fixture) {
  std::vector<BYTE> bytes = ReadCorpusFile(RealJournal::RequirePathOf(fixture));
  REQUIRE(bytes.size() == fixture.file_size);
  return bytes;
}

// The time of a record, in whole microseconds of UTC since 1601-01-01: the
// precision of the independent reader.
std::uint64_t Microseconds(const Usn::Record& record) {
  return EntryUtcTicks(record.time) / ticks_per_microsecond;
}

std::wstring Widen(std::string_view ascii) {
  return {ascii.begin(), ascii.end()};
}

void CheckSample(const Usn::Record& got, const JournalSample& want) {
  INFO("sample " << want.index << ", " << want.name);
  CHECK(got.usn == want.usn);
  CHECK(got.name == Widen(want.name));
  CHECK(got.file_record == want.file_record);
  CHECK(got.file_sequence == want.file_sequence);
  CHECK(got.parent_record == want.parent_record);
  CHECK(got.parent_sequence == want.parent_sequence);
  CHECK(got.reason == want.reason);
  CHECK(got.file_attributes == want.attributes);
  CHECK(Microseconds(got) == want.utc_microseconds);
}

// Whether two journal records say the same thing.
bool SameRecord(const Usn::Record& left, const Usn::Record& right) {
  return left.usn == right.usn && left.name == right.name &&
         left.file_record == right.file_record &&
         left.file_sequence == right.file_sequence &&
         left.parent_record == right.parent_record &&
         left.parent_sequence == right.parent_sequence &&
         left.reason == right.reason &&
         left.file_attributes == right.file_attributes &&
         left.major_version == right.major_version &&
         EntryUtcTicks(left.time) == EntryUtcTicks(right.time);
}

bool UsnsAscend(const std::vector<Usn::Record>& records) {
  return std::ranges::adjacent_find(records,
                                    [](const auto& before, const auto& after) {
                                      return before.usn >= after.usn;
                                    }) == records.end();
}

// Reads a whole journal in recovery mode, without caching: the setting the
// scans of a real file use.
Usn::Journal ReadRecovered(std::span<const BYTE> stream, ULONGLONG head) {
  std::optional<Usn::Journal> journal = RealJournal::ReadJournalOf(
      stream, head, Cache::Strategy::NoCache, RecoverOptions());
  REQUIRE(journal.has_value());
  return std::move(*journal);
}

}  // namespace

TEST_CASE("A real $J gives the independent record count and sample",
          "[dfir-corpora][integration][usn]") {
  const Fixture fixture = GENERATE(from_range(RealJournal::Fixtures()));
  CAPTURE(fixture.label);
  const std::vector<BYTE> stream = BytesOf(fixture);

  const Usn::Journal journal = ReadRecovered(stream, fixture.base_usn);

  REQUIRE(journal.records.size() == fixture.record_count);
  CHECK(journal.skipped_records == fixture.range_record_count);
  CHECK(journal.damaged_records == 0);
  CHECK_FALSE(journal.truncated);
  CHECK(UsnsAscend(journal.records));
  CHECK(journal.records.front().usn == static_cast<LONGLONG>(fixture.base_usn));
  // Only the real bytes cost a read: the stream is padded to a cluster, and
  // the sparse head costs none.
  CHECK(journal.scanned_bytes >= fixture.file_size);
  CHECK(journal.scanned_bytes < fixture.file_size + cluster_bytes);
  for (const Usn::Record& record : journal.records) {
    REQUIRE(record.major_version == 2);
  }
  for (const JournalSample& sample : fixture.samples) {
    REQUIRE(sample.index < journal.records.size());
    CheckSample(journal.records.at(sample.index), sample);
  }
}

TEST_CASE("The smallest real journals read alike under both cache strategies",
          "[dfir-corpora][integration][usn]") {
  const Fixture fixture = GENERATE(RealJournal::FixtureNamed("dfir_ntfs"),
                                   RealJournal::FixtureNamed("Server 2022"));
  CAPTURE(fixture.label);
  const std::vector<BYTE> stream = BytesOf(fixture);

  const std::optional<Usn::Journal> without_cache = RealJournal::ReadJournalOf(
      stream, fixture.base_usn, Cache::Strategy::NoCache, RecoverOptions());
  const std::optional<Usn::Journal> with_cache = RealJournal::ReadJournalOf(
      stream, fixture.base_usn, Cache::Strategy::FullCache, RecoverOptions());

  REQUIRE(without_cache.has_value());
  REQUIRE(with_cache.has_value());
  REQUIRE(with_cache->records.size() == fixture.record_count);
  REQUIRE(without_cache->records.size() == fixture.record_count);
  CHECK(with_cache->damaged_records == without_cache->damaged_records);
  CHECK(with_cache->skipped_records == without_cache->skipped_records);
  for (std::size_t i = 0; i < fixture.record_count; ++i) {
    REQUIRE(
        SameRecord(with_cache->records.at(i), without_cache->records.at(i)));
  }
}

TEST_CASE("Strict mode does not read a real journal",
          "[dfir-corpora][integration][usn][strict]") {
  const Fixture& fixture = RealJournal::FixtureNamed("dfir_ntfs");
  const std::vector<BYTE> stream = BytesOf(fixture);

  for (const VolumeOptions& options :
       {VolumeOptions{}, VolumeOptions{.include_deleted = true}}) {
    for (const Cache::Strategy strategy :
         {Cache::Strategy::NoCache, Cache::Strategy::FullCache}) {
      CHECK_FALSE(
          RealJournal::ReadJournalOf(stream, 0, strategy, options).has_value());
    }
  }
}

TEST_CASE("A real $J keeps its USNs wherever the stream is served",
          "[dfir-corpora][integration][usn]") {
  // The Windows 10 journal starts at USN 92 274 688: NTFS purged what came
  // before. A stream served from offset 0 puts every USN away from its offset.
  const Fixture& fixture = RealJournal::FixtureNamed("Windows 10");
  REQUIRE(fixture.base_usn != 0);
  const std::vector<BYTE> whole = BytesOf(fixture);
  // The bytes before the second sample are exactly the records before it.
  const JournalSample& cut = fixture.samples[1];
  const std::span<const BYTE> prefix =
      std::span<const BYTE>(whole).first(cut.offset);

  const Usn::Journal after_hole = ReadRecovered(prefix, fixture.base_usn);
  const Usn::Journal from_zero = ReadRecovered(prefix, 0);

  REQUIRE(after_hole.records.size() == cut.index);
  REQUIRE(from_zero.records.size() == cut.index);
  CHECK(after_hole.damaged_records == 0);
  CHECK(from_zero.damaged_records == 0);
  CHECK(from_zero.records.front().usn ==
        static_cast<LONGLONG>(fixture.base_usn));
  for (std::size_t i = 0; i < cut.index; ++i) {
    REQUIRE(SameRecord(after_hole.records.at(i), from_zero.records.at(i)));
  }
  // The scan of a sparse head reads nothing of it.
  CHECK(from_zero.scanned_bytes == after_hole.scanned_bytes);
}

TEST_CASE("The dfir_ntfs journal matches its fsutil listing record by record",
          "[dfir-corpora][integration][usn]") {
  const Fixture& fixture = RealJournal::FixtureNamed("dfir_ntfs");
  const std::vector<BYTE> stream = BytesOf(fixture);
  const RealJournal::FsutilListing listing = RealJournal::ParseFsutilListing(
      RequireCorpusFile(DfirNtfsDir() / "test_data" / "usnjrnlj.fsutil.txt"));
  const Usn::Journal journal = ReadRecovered(stream, 0);

  REQUIRE(listing.records.size() == fsutil_record_count);
  std::unordered_map<LONGLONG, const RealJournal::FsutilRecord*> by_usn;
  std::size_t listed_without_name = 0;
  for (const RealJournal::FsutilRecord& listed : listing.records) {
    by_usn.emplace(listed.usn, &listed);
    listed_without_name += listed.has_name ? 0 : 1;
  }
  REQUIRE(by_usn.size() == listing.records.size());
  // The V4 records have no name, and the library counts them apart.
  CHECK(listed_without_name == fsutil_range_records);
  CHECK(journal.skipped_records == fsutil_range_records);

  std::size_t compared = 0;
  std::size_t written_later = 0;
  for (const Usn::Record& record : journal.records) {
    const auto found = by_usn.find(record.usn);
    if (found == by_usn.end()) {
      // fsutil ran before NTFS wrote this one.
      CHECK(static_cast<ULONGLONG>(record.usn) >= listing.next_usn);
      ++written_later;
      continue;
    }
    const RealJournal::FsutilRecord& listed = *found->second;
    INFO("usn " << record.usn << ", " << listed.name);
    CHECK(listed.has_name);
    CHECK(Widen(listed.name) == record.name);
    CHECK(listed.name_length == record.name.size() * sizeof(char16_t));
    CHECK(listed.reason == record.reason);
    CHECK(listed.attributes == record.file_attributes);
    // The listing is in the time zone of the machine that made it, UTC here:
    // the first record reads 21:36:10 and its FILETIME 21:36:10.924361 UTC.
    // EntryUtcSeconds() undoes the library's conversion to local time.
    CHECK(listed.seconds == EntryUtcSeconds(record.time));
    CHECK(listed.file_id_low ==
          ((ULONGLONG{record.file_sequence} << 48U) | record.file_record));
    CHECK(listed.file_id_high == record.file_reference_high);
    CHECK(listed.parent_id_low ==
          ((ULONGLONG{record.parent_sequence} << 48U) | record.parent_record));
    CHECK(listed.parent_id_high == record.parent_reference_high);
    ++compared;
  }
  CHECK(written_later == records_after_fsutil);
  // Every named block of the listing is one record of the journal.
  CHECK(compared == listing.records.size() - fsutil_range_records);
  CHECK(compared + written_later == fixture.record_count);
}

namespace {

// The intact journal, read once for all the tests that damage it.
const Usn::Journal& IntactServer2012() {
  static const Usn::Journal intact = [] {
    const Fixture& fixture = RealJournal::FixtureNamed("Server 2012 R2");
    const Usn::Journal journal =
        ReadRecovered(BytesOf(fixture), fixture.base_usn);
    REQUIRE(journal.records.size() == fixture.record_count);
    return journal;
  }();
  return intact;
}

// The records of baseline whose bytes lie wholly outside [begin, end). A
// record ends where the next one starts, or where the stream ends: padding
// between two records counts as the first one's.
std::vector<const Usn::Record*> Untouched(const Usn::Journal& baseline,
                                          std::uint64_t stream_size,
                                          std::uint64_t begin,
                                          std::uint64_t end) {
  std::vector<const Usn::Record*> kept;
  for (std::size_t i = 0; i < baseline.records.size(); ++i) {
    const Usn::Record& record = baseline.records.at(i);
    const auto start = static_cast<std::uint64_t>(record.usn);
    const std::uint64_t stop =
        i + 1 < baseline.records.size()
            ? static_cast<std::uint64_t>(baseline.records.at(i + 1).usn)
            : stream_size;
    if (stop <= begin || start >= end) {
      kept.push_back(&record);
    }
  }
  return kept;
}

// Checks that found holds every record of kept unchanged, and nothing that
// the intact journal does not hold at the same USN.
void CheckSurvivors(const Usn::Journal& found, const Usn::Journal& baseline,
                    const std::vector<const Usn::Record*>& kept) {
  std::unordered_map<LONGLONG, const Usn::Record*> in_found;
  for (const Usn::Record& record : found.records) {
    in_found.emplace(record.usn, &record);
  }
  for (const Usn::Record* record : kept) {
    const auto match = in_found.find(record->usn);
    INFO("usn " << record->usn << " is lost");
    REQUIRE(match != in_found.end());
    REQUIRE(SameRecord(*match->second, *record));
  }

  std::unordered_map<LONGLONG, bool> in_baseline;
  for (const Usn::Record& record : baseline.records) {
    in_baseline.emplace(record.usn, true);
  }
  for (const Usn::Record& record : found.records) {
    INFO("usn " << record.usn << " is no record of the intact journal");
    REQUIRE(in_baseline.contains(record.usn));
  }
  CHECK(UsnsAscend(found.records));
}

// The sample of the intact journal at position index of its list of 20.
const JournalSample& SampleOf(std::size_t index) {
  return RealJournal::FixtureNamed("Server 2012 R2").samples[index];
}

}  // namespace

TEST_CASE("Damage to a real $J loses only the records it touches",
          "[dfir-corpora][integration][usn]") {
  const Fixture& fixture = RealJournal::FixtureNamed("Server 2012 R2");
  const Usn::Journal& baseline = IntactServer2012();
  std::vector<BYTE> stream = BytesOf(fixture);
  const std::uint64_t size = stream.size();

  SECTION("zeros over the start, ending on a record") {
    const JournalSample& first_kept = SampleOf(3);
    std::fill_n(stream.begin(), static_cast<std::ptrdiff_t>(first_kept.offset),
                BYTE{0});

    const Usn::Journal found = ReadRecovered(stream, 0);

    REQUIRE(found.records.size() == fixture.record_count - first_kept.index);
    CHECK(found.records.front().usn == first_kept.usn);
    // Zeros are padding, not damage.
    CHECK(found.damaged_records == 0);
    CheckSurvivors(found, baseline,
                   Untouched(baseline, size, 0, first_kept.offset));
  }

  SECTION("zeros over the start, ending inside a record") {
    const JournalSample& hit = SampleOf(3);
    const std::uint64_t end = hit.offset + into_record;
    std::fill_n(stream.begin(), static_cast<std::ptrdiff_t>(end), BYTE{0});

    const Usn::Journal found = ReadRecovered(stream, 0);

    CheckSurvivors(found, baseline, Untouched(baseline, size, 0, end));
    // The record the zeros end in is the only one lost besides the zeroed.
    CHECK(found.records.size() <= fixture.record_count - hit.index - 1);
    CHECK(found.damaged_records >= 1);
  }

  SECTION("the head cut off inside a record, as a purge leaves it") {
    // NTFS frees whole clusters from the head: the record that straddles the
    // first kept cluster loses its start.
    const std::uint64_t head = (SampleOf(3).offset + 8 + cluster_bytes - 1) /
                               cluster_bytes * cluster_bytes;
    const std::vector<BYTE> tail(
        stream.begin() + static_cast<std::ptrdiff_t>(head), stream.end());

    const Usn::Journal found = ReadRecovered(tail, head);

    const auto kept = Untouched(baseline, size, 0, head);
    CheckSurvivors(found, baseline, kept);
    // Only the record that straddles the cut is lost.
    CHECK(found.records.size() == kept.size());
    CHECK(found.records.front().usn >= static_cast<LONGLONG>(head));
  }

  SECTION("the end cut inside a record") {
    const JournalSample& hit = SampleOf(15);
    stream.resize(hit.offset + cut_into_record);

    const Usn::Journal found = ReadRecovered(stream, 0);

    REQUIRE(found.records.size() == hit.index);
    CHECK(found.damaged_records >= 1);
    CheckSurvivors(found, baseline,
                   Untouched(baseline, size, hit.offset, size));
  }

  SECTION("zeros over whole records in the middle") {
    const JournalSample& from = SampleOf(6);
    const JournalSample& to = SampleOf(9);
    std::fill(stream.begin() + static_cast<std::ptrdiff_t>(from.offset),
              stream.begin() + static_cast<std::ptrdiff_t>(to.offset), BYTE{0});

    const Usn::Journal found = ReadRecovered(stream, 0);

    REQUIRE(found.records.size() ==
            fixture.record_count - (to.index - from.index));
    CHECK(found.damaged_records == 0);
    CheckSurvivors(found, baseline,
                   Untouched(baseline, size, from.offset, to.offset));
  }

  SECTION("zeros from inside a record to inside another") {
    const JournalSample& from = SampleOf(6);
    const JournalSample& to = SampleOf(9);
    const std::uint64_t begin = from.offset + into_record;
    const std::uint64_t end = to.offset + into_record;
    std::fill(stream.begin() + static_cast<std::ptrdiff_t>(begin),
              stream.begin() + static_cast<std::ptrdiff_t>(end), BYTE{0});

    const Usn::Journal found = ReadRecovered(stream, 0);

    const auto kept = Untouched(baseline, size, begin, end);
    CheckSurvivors(found, baseline, kept);
    // The two records the zeros cut into are the only ones that may be lost
    // or altered besides those in the zeroed range.
    CHECK(found.records.size() >= kept.size());
    CHECK(found.damaged_records >= 1);
  }
}
