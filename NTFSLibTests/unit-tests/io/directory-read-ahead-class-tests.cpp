#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/disk-readers/throttled-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "attr/mask.h"
#include "cache/strategy.h"
#include "directory-read-ahead-support.h"
#include "io/directory-read-ahead.h"
#include "io/file-record.h"
#include "log/ntfs-common.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "tuning/tuning.h"
#include "volume-options.h"
#include "win-types.h"

namespace NTFS_BROWSER_NAMESPACE {

class IndexEntryView;

namespace Io {

enum class RecordStanding : std::uint8_t;

}  // namespace Io
template <Cache::Strategy S>
class NtfsVolume;

}  // namespace NTFS_BROWSER_NAMESPACE

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestIo = NTFS_BROWSER_NAMESPACE::Test::Io;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using TestDiskReaders::CountingDiskReader;

namespace {

// Directories the big trees of these tests hold.
constexpr std::size_t big_directories = 6;

// A budget that never binds.
constexpr std::uint64_t large_budget = std::uint64_t{16} << 20U;

// The record number of big directory number.
ULONGLONG BigDirectory(std::size_t number) {
  return TestFakeImage::name_tree_first_record +
         (number * TestIo::big_directory_nodes);
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
  TestDiskReaders::SimulatedClock clock;
  CountingDiskReader* reads{nullptr};
  const TestDiskReaders::ThrottledDiskReader* disk{nullptr};
  std::unique_ptr<NtfsVolume<S>> volume;
  std::unique_ptr<Io::FileRecord<S>> listing;

  Rig(const std::vector<BYTE>& image, VolumeOptions options, std::size_t dirs,
      std::uint64_t bytes, const TestDiskReaders::DiskModel& model = {}) {
    options.tuning.walk.read_ahead_dirs = dirs;
    options.tuning.walk.read_ahead_bytes = bytes;
    auto throttled = std::make_unique<TestDiskReaders::ThrottledDiskReader>(
        std::make_unique<TestDiskReaders::MemoryDiskReader>(image), model,
        clock, options.tuning.reader);
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
    (void)TestSupport::TakeCapturedLog();
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
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
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
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
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
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, large_budget, TestIo::HardDiskModel());
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
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
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
    CHECK(ListEntries(*rig.listing) == TestIo::big_directory_files + 1);
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
  const TestIo::MftSpan mft = TestIo::MftSpanOf(image);
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
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
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
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
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
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
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
  const auto fixtures = TestIo::Fixtures();
  const auto spoiled =
      std::ranges::find_if(fixtures, [](const TestIo::TreeFixture& fixture) {
        return fixture.name == "unreadable directory";
      });
  REQUIRE(spoiled != fixtures.end());
  const auto damaged =
      std::ranges::find_if(fixtures, [](const TestIo::TreeFixture& fixture) {
        return fixture.name == "damaged block";
      });
  REQUIRE(damaged != fixtures.end());

  for (const TestIo::TreeFixture* fixture : {&*spoiled, &*damaged}) {
    INFO(fixture->name);
    Rig<S> rig(fixture->image, {.recover_errors = true}, 64, large_budget);
    Io::DirectoryReadAhead<S> ahead(*rig.volume);
    ahead.Attach(*rig.listing);
    std::vector<ULONGLONG> records;
    // Nodes both trees hold: a run of records MUST NOT reach past $MFT.
    for (std::size_t node = 0; node < 30; node++) {
      records.push_back(TestFakeImage::name_tree_first_record + node);
    }
    (void)TestSupport::TakeCapturedLog();
    const std::uint64_t salvaged = Log::Detail::RecoveredCount();

    ahead.Plan(records);

    CHECK(ahead.GetStats().records_read > 0);
    CHECK(TestSupport::TakeCapturedLog().empty());
    CHECK(Log::Detail::RecoveredCount() == salvaged);
  }
}

TEMPLATE_TEST_CASE_SIG("The read ahead reads the first blocks of the start",
                       "[io][directory-read-ahead]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
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
  CHECK(ListEntries(*rig.listing) == TestIo::big_directory_files + 1);
  CHECK(ahead.GetStats().blocks_served == stats.blocks_read);
  // The blocks came from the read ahead: only the probes of the entries read.
  const TestIo::MftSpan mft = TestIo::MftSpanOf(image);
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
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
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
      TestFakeImage::BuildFakeNtfsImageWithNameTree(TestIo::WideTree(40));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  // Directory number d of the wide tree is node 4 * d.
  const auto directory = [](std::size_t number) {
    return TestFakeImage::name_tree_first_record + (4 * number);
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

TEMPLATE_TEST_CASE_SIG(
    "The read ahead reads the next directory alone when it is not held, "
    "however full the window",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image =
      TestFakeImage::BuildFakeNtfsImageWithNameTree(TestIo::WideTree(40));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  // Directory number d of the wide tree is node 4 * d.
  const auto directory = [](std::size_t number) {
    return TestFakeImage::name_tree_first_record + (4 * number);
  };
  std::vector<ULONGLONG> window;
  for (std::size_t number = 0; number < 8; number++) {
    window.push_back(directory(number));
  }
  ahead.Plan(window);
  REQUIRE(ahead.GetStats().batches == 1);
  REQUIRE(ahead.GetStats().records_read == 8);

  // Seven of eight are held, and the one the listing enters next is not: a
  // batch of that one.
  ahead.Release(directory(0));
  ahead.Plan(window);
  CHECK(ahead.GetStats().batches == 2);
  CHECK(ahead.GetStats().records_read == 9);

  // It is held now: nothing more to read.
  ahead.Plan(window);
  CHECK(ahead.GetStats().batches == 2);

  // Seven held, and the one that is not held comes last: no batch.
  ahead.Release(directory(1));
  window = {directory(2), directory(3), directory(4), directory(5),
            directory(6), directory(7), directory(0), directory(8)};
  ahead.Plan(window);
  CHECK(ahead.GetStats().batches == 2);
  CHECK(ahead.GetStats().records_read == 9);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead reads only the next directory when others wait behind it",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image =
      TestFakeImage::BuildFakeNtfsImageWithNameTree(TestIo::WideTree(40));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  const auto directory = [](std::size_t number) {
    return TestFakeImage::name_tree_first_record + (4 * number);
  };
  std::vector<ULONGLONG> held;
  for (std::size_t number = 0; number < 4; number++) {
    held.push_back(directory(number));
  }
  ahead.Plan(held);
  REQUIRE(ahead.GetStats().records_read == 4);

  // Four of eight held is half, which is enough. Three directories that are
  // not held wait behind the next one, which is not held either: only the next
  // is read.
  std::vector<ULONGLONG> window{directory(10), directory(11), directory(12),
                                directory(13)};
  window.insert(window.end(), held.begin(), held.end());
  ahead.Plan(window);

  CHECK(ahead.GetStats().batches == 2);
  CHECK(ahead.GetStats().records_read == 5);
}

TEMPLATE_TEST_CASE_SIG(
    "The listing takes the blocks of a directory the read ahead read alone",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  std::vector<ULONGLONG> window;
  for (std::size_t number = 1; number < big_directories; number++) {
    window.push_back(BigDirectory(number));
  }
  ahead.Plan(window);
  const Io::DirectoryReadAheadStats before = ahead.GetStats();
  REQUIRE(before.records_read == big_directories - 1);

  // Five held, and the next directory is the one that is not.
  window.insert(window.begin(), BigDirectory(0));
  ahead.Plan(window);

  const Io::DirectoryReadAheadStats planned = ahead.GetStats();
  CHECK(planned.batches == before.batches + 1);
  CHECK(planned.records_read == before.records_read + 1);
  CHECK(planned.blocks_read > before.blocks_read);
  rig.reads->Reset();
  REQUIRE(rig.listing->ParseFileRecord(BigDirectory(0)));
  REQUIRE(rig.listing->ParseAttrs());
  CHECK(ListEntries(*rig.listing) == TestIo::big_directory_files + 1);
  ahead.Release(BigDirectory(0));

  // The listing read no block of it, only the probes of its entries.
  const Io::DirectoryReadAheadStats done = ahead.GetStats();
  CHECK(done.blocks_served - before.blocks_served ==
        planned.blocks_read - before.blocks_read);
  CHECK(done.records_served == before.records_served + 1);
  const TestIo::MftSpan mft = TestIo::MftSpanOf(image);
  CHECK(std::ranges::none_of(
      rig.reads->Calls(), [&](const CountingDiskReader::Call& call) {
        return call.kind == CountingDiskReader::Kind::Read &&
               call.offset >= mft.end;
      }));
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead reads the next directory within its budget only",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
  std::vector<ULONGLONG> window;
  for (std::size_t number = 1; number < big_directories; number++) {
    window.push_back(BigDirectory(number));
  }
  std::uint64_t held_bytes = 0;
  {
    Rig<S> rig(image, {}, 8, large_budget);
    Io::DirectoryReadAhead<S> ahead(*rig.volume);
    ahead.Attach(*rig.listing);
    ahead.Plan(window);
    held_bytes = ahead.GetStats().held_bytes;
  }
  // Room for what five directories hold, and less than a record more.
  const std::uint64_t budget = held_bytes + 512;
  Rig<S> rig(image, {}, 8, budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  ahead.Plan(window);
  const Io::DirectoryReadAheadStats before = ahead.GetStats();
  REQUIRE(before.records_read == big_directories - 1);

  window.insert(window.begin(), BigDirectory(0));
  ahead.Plan(window);

  CHECK(ahead.GetStats().batches == before.batches);
  CHECK(ahead.GetStats().held_bytes <= budget);
  CHECK(ahead.GetStats().peak_held_bytes <= budget);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead reads only the record of the next directory under a raw "
    "attribute callback",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
  Rig<S> rig(image, {}, 8, large_budget);
  REQUIRE(rig.volume->InstallAttrRawCB(
      NTFS_BROWSER_NAMESPACE::Attr::Type::FileName,
      [](const NTFS_BROWSER_NAMESPACE::Attr::HeaderCommon&, bool& discard) {
        discard = false;
      }));
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  std::vector<ULONGLONG> window;
  for (std::size_t number = 1; number < big_directories; number++) {
    window.push_back(BigDirectory(number));
  }
  ahead.Plan(window);
  const Io::DirectoryReadAheadStats before = ahead.GetStats();
  REQUIRE(before.records_read == big_directories - 1);
  REQUIRE(before.blocks_read == 0);

  window.insert(window.begin(), BigDirectory(0));
  ahead.Plan(window);

  CHECK(ahead.GetStats().batches == before.batches + 1);
  CHECK(ahead.GetStats().records_read == before.records_read + 1);
  CHECK(ahead.GetStats().blocks_read == 0);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead holds every level of the index of the start directory",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // The blocks of the root chain, each naming the next one, past the depth
  // limit of the walk.
  const auto image = TestFakeImage::BuildFakeNtfsImageWithDeepIndexBlockChain();
  Rig<S> rig(image, {}, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  ahead.Begin();

  // The walk enters a block of depth 63 at the deepest, and none below.
  constexpr std::uint64_t walk_depth_limit = 64;
  const Io::DirectoryReadAheadStats stats = ahead.GetStats();
  CHECK(stats.blocks_read == walk_depth_limit);
  CHECK(stats.held_bytes == walk_depth_limit * rig.volume->GetIndexBlockSize());
  rig.reads->Reset();
  (void)ListEntries(*rig.listing);
  CHECK(ahead.GetStats().blocks_served == stats.blocks_read);
  // Every block came from the read ahead: the listing read none.
  CHECK(std::ranges::none_of(
      rig.reads->Calls(), [](const CountingDiskReader::Call& call) {
        return call.kind == CountingDiskReader::Kind::Read;
      }));
  ahead.Release(static_cast<ULONGLONG>(Mft::Idx::Root));
  CHECK(ahead.GetStats().held_bytes == 0);
  CHECK(ahead.GetStats().blocks_unused == 0);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead holds a quarter of its budget for the deeper blocks of a "
    "directory",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = TestFakeImage::BuildFakeNtfsImageWithDeepIndexBlockChain();
  std::uint32_t block_size = 0;
  {
    Rig<S> rig(image, {}, 8, large_budget);
    block_size = rig.volume->GetIndexBlockSize();
  }
  REQUIRE(block_size > 0);

  // The first block, and room for one more below it.
  Rig<S> rig(image, {}, 8, 8 * std::uint64_t{block_size});
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);

  ahead.Begin();

  const Io::DirectoryReadAheadStats stats = ahead.GetStats();
  CHECK(stats.blocks_read == 2);
  CHECK(stats.held_bytes == 2 * std::uint64_t{block_size});
  // The listing reads the others itself, and lists what it listed without
  // the read ahead.
  CHECK(ListEntries(*rig.listing) == 0);
  CHECK(ahead.GetStats().blocks_served == 2);
}

TEST_CASE("The record memo gives the probes of a listing the records it kept",
          "[io][directory-read-ahead]") {
  constexpr Cache::Strategy S = Cache::Strategy::NoCache;
  const auto image = TestFakeImage::BuildFakeNtfsImageWithNameTree(
      TestIo::BigDirectoryTree(big_directories));
  // The window would serve the second listing too: leave it out.
  VolumeOptions options;
  options.tuning.walk.record_window_bytes = 0;
  Rig<S> rig(image, options, 8, large_budget);
  Io::DirectoryReadAhead<S> ahead(*rig.volume);
  ahead.Attach(*rig.listing);
  REQUIRE(rig.listing->ParseFileRecord(BigDirectory(1)));
  REQUIRE(rig.listing->ParseAttrs());

  rig.reads->Reset();
  const std::size_t first_entries = ListEntries(*rig.listing);
  const std::size_t first_reads = rig.reads->Calls().size();
  const std::uint64_t first_hits = ahead.GetStats().memo_records;

  rig.reads->Reset();
  CHECK(ListEntries(*rig.listing) == first_entries);
  CHECK(rig.reads->Calls().size() < first_reads);
  CHECK(ahead.GetStats().memo_records > first_hits);
}
