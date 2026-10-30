#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/index-entry.h>
#include <ntfs-browser/io/directory-read-ahead.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/io/record-standing.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "directory-read-ahead-support.h"
#include "fake-ntfs-image.h"
#include "log/ntfs-common.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"
#include "throttled-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace ReadAhead = NTFS_BROWSER_NAMESPACE::Tests::ReadAhead;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::CountingDiskReader;

namespace {

// Directories the big trees of these tests hold.
constexpr std::size_t big_directories = 6;

// A budget that never binds.
constexpr std::uint64_t large_budget = std::uint64_t{16} << 20U;

// The record number of big directory number.
ULONGLONG BigDirectory(std::size_t number) {
  return Tests::name_tree_first_record +
         (number * ReadAhead::big_directory_nodes);
}

// All of them, the next one first.
std::vector<ULONGLONG> AllBigDirectories() {
  std::vector<ULONGLONG> records;
  for (std::size_t number = 0; number < big_directories; number++) {
    records.push_back(BigDirectory(number));
  }
  return records;
}

// A volume over a counting reader over a simulated disk, and a record that
// parses its root: what a listing starts from.
template <Cache::Strategy S>
struct Rig {
  Tests::SimulatedClock clock;
  CountingDiskReader* reads{nullptr};
  const Tests::ThrottledDiskReader* disk{nullptr};
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<Io::FileRecord<S>> listing;

  Rig(const std::vector<BYTE>& image, VolumeOptions options, std::size_t dirs,
      std::uint64_t bytes, const Tests::DiskModel& model = {}) {
    options.tuning.walk.read_ahead_dirs = dirs;
    options.tuning.walk.read_ahead_bytes = bytes;
    auto throttled = std::make_unique<Tests::ThrottledDiskReader>(
        std::make_unique<Tests::MemoryDiskReader>(image), model, clock,
        options.tuning.reader);
    disk = throttled.get();
    auto counting = std::make_unique<CountingDiskReader>(std::move(throttled));
    reads = counting.get();
    volume = std::make_unique<NtfsVolume<S>>(std::move(counting), options);
    REQUIRE(volume->IsVolumeOK());
    listing = std::make_unique<Io::FileRecord<S>>(*volume);
    listing->SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                         NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
    REQUIRE(listing->ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
    REQUIRE(listing->ParseAttrs());
    reads->Reset();
    (void)Tests::TakeCapturedLog();
  }
};

// The reads and hints of calls, cut into rounds: a round ends where the
// announcements that nothing used are withdrawn.
using Round = std::vector<CountingDiskReader::Call>;

std::vector<Round> Rounds(const std::vector<CountingDiskReader::Call>& calls) {
  std::vector<Round> rounds(1);
  for (const CountingDiskReader::Call& call : calls) {
    if (call.kind == CountingDiskReader::Kind::CancelPrefetch) {
      rounds.emplace_back();
    } else {
      rounds.back().push_back(call);
    }
  }
  return rounds;
}

// Lists a directory the listing record holds, as the listing of NtfsDir does,
// and counts its entries.
template <Cache::Strategy S>
std::size_t ListEntries(Io::FileRecord<S>& listing) {
  std::size_t entries = 0;
  listing.TraverseSubEntriesWithStanding(
      [](const IndexEntryView&, Io::RecordStanding, void* context) {
        ++*static_cast<std::size_t*>(context);
      },
      &entries);
  return entries;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "The read ahead holds the records and the first blocks of the planned "
    "directories",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  REQUIRE(ahead.Enabled());
  CHECK(ahead.Window() == 8);
  ahead.Attach(*rig.listing);

  ahead.Plan(AllBigDirectories());

  const Io::DirectoryReadAheadStats stats = ahead.GetStats();
  CHECK(stats.batches == 1);
  CHECK(stats.records_read == big_directories);
  CHECK(stats.blocks_read > big_directories);
  CHECK(stats.held_bytes ==
        (stats.records_read * rig.volume->GetFileRecordSize()) +
            (stats.blocks_read * rig.volume->GetIndexBlockSize()));
  CHECK(stats.peak_held_bytes == stats.held_bytes);
  CHECK(stats.records_served == 0);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead announces each batch before it reads it, in one sweep",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  ahead.Plan(AllBigDirectories());

  // The records of the directories, then their blocks.
  const std::vector<Round> rounds = Rounds(rig.reads->Calls());
  CHECK(rounds.size() >= 3);
  std::size_t reads = 0;
  for (const Round& round : rounds) {
    std::vector<CountingDiskReader::Call> announced;
    std::optional<ULONGLONG> previous;
    for (const CountingDiskReader::Call& call : round) {
      if (call.kind == CountingDiskReader::Kind::Prefetch) {
        // A hint comes before any read of its round.
        CHECK(previous == std::nullopt);
        announced.push_back(call);
      } else if (call.kind == CountingDiskReader::Kind::Read) {
        reads++;
        // The read lies in a range announced in the round, and the reads go
        // up the disk.
        CHECK(std::ranges::any_of(
            announced, [&](const CountingDiskReader::Call& hint) {
              return call.offset >= hint.offset &&
                     call.offset + call.length <= hint.offset + hint.length;
            }));
        CHECK(call.offset >= previous.value_or(0));
        previous = call.offset;
      }
    }
  }
  CHECK(reads > 0);
}

TEMPLATE_TEST_CASE_SIG("The read ahead withdraws no announcement unused",
                       "[io][directory-read-ahead]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, large_budget, ReadAhead::HardDiskModel());
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  // The volume was opened and its root parsed before.
  const std::uint64_t unannounced = rig.disk->Stats().unannounced_reads;

  ahead.Plan(AllBigDirectories());

  CHECK(ahead.GetStats().blocks_read > 0);
  CHECK(rig.disk->Stats().wasted_ranges == 0);
  CHECK(rig.disk->Stats().unannounced_reads == unannounced);
}

TEMPLATE_TEST_CASE_SIG(
    "The listing takes everything the read ahead held, and nothing stays held",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  ahead.Plan(AllBigDirectories());
  const Io::DirectoryReadAheadStats planned = ahead.GetStats();
  rig.reads->Reset();

  for (const ULONGLONG record : AllBigDirectories()) {
    REQUIRE(rig.listing->ParseFileRecord(record));
    REQUIRE(rig.listing->ParseAttrs());
    // The files and the subdirectory.
    CHECK(ListEntries(*rig.listing) == ReadAhead::big_directory_files + 1);
    ahead.Release(record);
  }

  const Io::DirectoryReadAheadStats done = ahead.GetStats();
  CHECK(done.records_served == planned.records_read);
  CHECK(done.blocks_served == planned.blocks_read);
  CHECK(done.records_unused == 0);
  CHECK(done.blocks_unused == 0);
  CHECK(done.held_bytes == 0);
  // The listing of these directories took their blocks from the read ahead, and
  // read only the records of their entries: no read starts past $MFT.
  const ReadAhead::MftSpan mft = ReadAhead::MftSpanOf(image);
  CHECK(std::ranges::none_of(
      rig.reads->Calls(), [&](const CountingDiskReader::Call& call) {
        return call.kind == CountingDiskReader::Kind::Read &&
               call.offset >= mft.end;
      }));
}

TEMPLATE_TEST_CASE_SIG(
    "A directory released unused is counted, and frees its bytes",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  ahead.Plan(AllBigDirectories());
  const Io::DirectoryReadAheadStats planned = ahead.GetStats();

  for (const ULONGLONG record : AllBigDirectories()) {
    ahead.Release(record);
  }
  // A directory that is not held is ignored.
  ahead.Release(BigDirectory(0));

  const Io::DirectoryReadAheadStats done = ahead.GetStats();
  CHECK(done.held_bytes == 0);
  CHECK(done.peak_held_bytes == planned.held_bytes);
  CHECK(done.records_unused == planned.records_read);
  CHECK(done.blocks_unused == planned.blocks_read);
}

TEMPLATE_TEST_CASE_SIG("The read ahead never holds more than its budget",
                       "[io][directory-read-ahead]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::BigDirectoryTree(big_directories));
  std::uint64_t unlimited_blocks = 0;
  {
    Rig<S> rig(image, {}, 8, large_budget);
    Io::DirectoryReadAhead<S> ahead(*rig.volume);
    ahead.Attach(*rig.listing);
    ahead.Plan(AllBigDirectories());
    unlimited_blocks = ahead.GetStats().blocks_read;
  }
  REQUIRE(unlimited_blocks > big_directories);

  // Room for the six records, and the blocks of one directory and a half.
  const std::uint64_t budget = 6 * std::uint64_t{1024} +
                               (unlimited_blocks / big_directories) * 1024 +
                               512;
  Rig<S> rig(image, {}, 8, budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  ahead.Plan(AllBigDirectories());

  const Io::DirectoryReadAheadStats stats = ahead.GetStats();
  CHECK(stats.records_read == big_directories);
  CHECK(stats.blocks_read > 0);
  CHECK(stats.blocks_read < unlimited_blocks);
  CHECK(stats.held_bytes <= budget);
  CHECK(stats.peak_held_bytes <= budget);

  // Once the listing has used some, planning goes on.
  for (const ULONGLONG record : AllBigDirectories()) {
    ahead.Release(record);
  }
  ahead.Plan(AllBigDirectories());
  CHECK(ahead.GetStats().peak_held_bytes <= budget);
  CHECK(ahead.GetStats().records_read == 2 * big_directories);
}

TEMPLATE_TEST_CASE_SIG("The read ahead plans nothing within a budget of zero",
                       "[io][directory-read-ahead]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, 0);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  ahead.Plan(AllBigDirectories());
  ahead.Begin();

  CHECK(ahead.GetStats().batches == 0);
  CHECK(rig.reads->Calls().empty());
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead logs nothing and leaves the salvage count alone",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto fixtures = ReadAhead::Fixtures();
  const auto spoiled =
      std::ranges::find_if(fixtures, [](const ReadAhead::TreeFixture& fixture) {
        return fixture.name == "unreadable directory";
      });
  REQUIRE(spoiled != fixtures.end());
  const auto damaged =
      std::ranges::find_if(fixtures, [](const ReadAhead::TreeFixture& fixture) {
        return fixture.name == "damaged block";
      });
  REQUIRE(damaged != fixtures.end());

  for (const ReadAhead::TreeFixture* fixture : {&*spoiled, &*damaged}) {
    INFO(fixture->name);
    Rig<S> rig(fixture->image, {.recover_errors = true}, 64, large_budget);
    Io::DirectoryReadAhead<S> ahead(*rig.volume);
    ahead.Attach(*rig.listing);
    std::vector<ULONGLONG> records;
    // Nodes both trees hold: a run of records MUST NOT reach past $MFT.
    for (std::size_t node = 0; node < 30; node++) {
      records.push_back(Tests::name_tree_first_record + node);
    }
    (void)Tests::TakeCapturedLog();
    const std::uint64_t salvaged = Log::Detail::RecoveredCount();

    ahead.Plan(records);

    CHECK(ahead.GetStats().records_read > 0);
    CHECK(Tests::TakeCapturedLog().empty());
    CHECK(Log::Detail::RecoveredCount() == salvaged);
  }
}

TEMPLATE_TEST_CASE_SIG("The read ahead reads the first blocks of the start",
                       "[io][directory-read-ahead]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  const ULONGLONG start = BigDirectory(2);
  REQUIRE(rig.listing->ParseFileRecord(start));
  REQUIRE(rig.listing->ParseAttrs());
  rig.reads->Reset();

  ahead.Begin();

  const Io::DirectoryReadAheadStats stats = ahead.GetStats();
  CHECK(stats.records_read == 0);
  CHECK(stats.blocks_read > 1);
  rig.reads->Reset();
  CHECK(ListEntries(*rig.listing) == ReadAhead::big_directory_files + 1);
  CHECK(ahead.GetStats().blocks_served == stats.blocks_read);
  // The blocks came from the read ahead: only the probes of the entries read.
  const ReadAhead::MftSpan mft = ReadAhead::MftSpanOf(image);
  CHECK(std::ranges::none_of(
      rig.reads->Calls(), [&](const CountingDiskReader::Call& call) {
        return call.kind == CountingDiskReader::Kind::Read &&
               call.offset >= mft.end;
      }));
  ahead.Release(start);
  CHECK(ahead.GetStats().held_bytes == 0);
}

TEMPLATE_TEST_CASE_SIG("The read ahead does nothing while it is off",
                       "[io][directory-read-ahead]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 0, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);

  ahead.Attach(*rig.listing);
  ahead.Begin();
  ahead.Plan(AllBigDirectories());
  ahead.Release(BigDirectory(0));

  CHECK_FALSE(ahead.Enabled());
  CHECK(ahead.Window() == 0);
  CHECK(rig.reads->Calls().empty());
  CHECK_FALSE(rig.listing->GetIndexBlockSource());
  const Io::DirectoryReadAheadStats stats = ahead.GetStats();
  CHECK(stats.batches == 0);
  CHECK(stats.held_bytes == 0);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead takes a half-held window as enough, and a thin one as a "
    "reason to read",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image =
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::WideTree(40));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  // Directory number d of the wide tree is node 4 * d.
  const auto directory = [](std::size_t number) {
    return Tests::name_tree_first_record + (4 * number);
  };
  std::vector<ULONGLONG> window;
  for (std::size_t number = 0; number < 8; number++) {
    window.push_back(directory(number));
  }

  ahead.Plan(window);
  CHECK(ahead.GetStats().batches == 1);
  CHECK(ahead.GetStats().records_read == 8);

  // Seven of eight are held: no batch for one directory.
  ahead.Release(directory(0));
  window.erase(window.begin());
  window.push_back(directory(8));
  ahead.Plan(window);
  CHECK(ahead.GetStats().batches == 1);

  // Down to three of eight held: the window is thin, and refilled in one go.
  for (std::size_t number = 1; number < 5; number++) {
    ahead.Release(directory(number));
  }
  window.clear();
  for (std::size_t number = 5; number < 13; number++) {
    window.push_back(directory(number));
  }
  ahead.Plan(window);
  CHECK(ahead.GetStats().batches == 2);
  CHECK(ahead.GetStats().records_read == 8 + 5);
}
