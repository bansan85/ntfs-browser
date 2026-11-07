#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/bench-session-guard.h>
#include <ntfs-browser/tuning/tuning.h>

#include "io/file-reader.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using Call = TestDiskReaders::CountingDiskReader::Call;
using Kind = TestDiskReaders::CountingDiskReader::Kind;

namespace {

// Bytes of the image the file reader reads: four blocks of the default cache.
constexpr size_t image_bytes = size_t{4} * 64 * 1024;

// The cache block of FullCache: the default of the tuning.
constexpr ULONGLONG block_bytes =
    NTFS_BROWSER_NAMESPACE::Tuning::Settings{}.cache.block_bytes;

// The largest offset of a withdrawal that means the whole lane.
constexpr ULONGLONG max_offset = (std::numeric_limits<ULONGLONG>::max)();

// A hint inside a block, and one across the end of a block.
constexpr ULONGLONG inside_offset = (3 * block_bytes) + 100;
constexpr ULONGLONG inside_length = 200;
constexpr ULONGLONG across_offset = (6 * block_bytes) - 50;
constexpr ULONGLONG across_length = 100;

// A file reader over a counting reader, and a pointer to the counting reader.
template <Cache::Strategy S>
struct CountedFile {
  std::unique_ptr<Io::FileReader<S>> file;
  const TestDiskReaders::CountingDiskReader* counting;
};

template <Cache::Strategy S>
CountedFile<S> MakeCountedFile() {
  auto counting = std::make_unique<TestDiskReaders::CountingDiskReader>(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(
          std::vector<BYTE>(image_bytes, BYTE{5})));
  const TestDiskReaders::CountingDiskReader* const pointer = counting.get();
  return {std::make_unique<Io::FileReader<S>>(std::move(counting)), pointer};
}

// The events of the log that have type, as calls: offset and length.
std::vector<Call> EventsOf(const Bench::ReadLog& log, Bench::EventType type,
                           Kind as) {
  std::vector<Call> events;
  for (const Bench::LogEvent& event : log.events) {
    if (event.type == type) {
      events.push_back(
          {.kind = as, .offset = event.offset, .length = event.length});
    }
  }
  return events;
}

// The calls of one kind, in order.
std::vector<Call> CallsOf(const std::vector<Call>& calls, Kind kind) {
  std::vector<Call> found;
  std::ranges::copy_if(calls, std::back_inserter(found),
                       [kind](const Call& call) { return call.kind == kind; });
  return found;
}

bool SameCalls(const std::vector<Call>& left, const std::vector<Call>& right) {
  return std::ranges::equal(left, right, [](const Call& lhs, const Call& rhs) {
    return lhs.kind == rhs.kind && lhs.offset == rhs.offset &&
           lhs.length == rhs.length;
  });
}

// Checks that the S and X events of the log are, in order, the calls of the
// standing lane that reached the reader.
void CheckLogMatchesReader(const Bench::ReadLog& log,
                           const std::vector<Call>& calls) {
  const std::vector<Call> announced = CallsOf(calls, Kind::PrefetchStanding);
  const std::vector<Call> withdrawn = CallsOf(calls, Kind::CancelStanding);
  CHECK(SameCalls(
      EventsOf(log, Bench::EventType::StandingPrefetch, Kind::PrefetchStanding),
      announced));
  CHECK(SameCalls(
      EventsOf(log, Bench::EventType::StandingCancel, Kind::CancelStanding),
      withdrawn));
}

}  // namespace

TEST_CASE("A session logs the standing lane of a FileReader<NoCache> as given",
          "[bench][standing]") {
  CountedFile<Cache::Strategy::NoCache> counted =
      MakeCountedFile<Cache::Strategy::NoCache>();

  TestSupport::BenchSessionGuard session;
  counted.file->PrefetchStanding(inside_offset, inside_length);
  counted.file->PrefetchStanding(across_offset, across_length);
  counted.file->PrefetchStanding(inside_offset, 0);
  counted.file->CancelStanding(inside_offset, 1);
  counted.file->CancelStanding(inside_offset, 0);
  counted.file->CancelStanding(0, max_offset);
  const Bench::Report report = session.Finish();
  const Bench::ReadLog log = Bench::TakeReadLog();

  const std::vector<Call> announced =
      EventsOf(log, Bench::EventType::StandingPrefetch, Kind::PrefetchStanding);
  REQUIRE(announced.size() == 2);
  CHECK(announced.at(0).offset == inside_offset);
  CHECK(announced.at(0).length == inside_length);
  CHECK(announced.at(1).offset == across_offset);
  CHECK(announced.at(1).length == across_length);
  const std::vector<Call> withdrawn =
      EventsOf(log, Bench::EventType::StandingCancel, Kind::CancelStanding);
  REQUIRE(withdrawn.size() == 2);
  CHECK(withdrawn.at(0).offset == inside_offset);
  CHECK(withdrawn.at(0).length == 1);
  CHECK(withdrawn.at(1).offset == 0);
  CHECK(withdrawn.at(1).length == max_offset);

  CHECK(report.standing_prefetch_calls == 2);
  CHECK(report.standing_prefetch_bytes == inside_length + across_length);
  CHECK(report.standing_cancel_calls == 2);
  // The first queue's counters do not see the lane.
  CHECK(report.prefetch_calls == 0);
  CHECK(report.prefetch_bytes == 0);
  CHECK(report.cancel_calls == 0);
  CheckLogMatchesReader(log, counted.counting->Calls());
}

TEST_CASE("A session logs the widened ranges of a FileReader<FullCache>",
          "[bench][standing]") {
  CountedFile<Cache::Strategy::FullCache> counted =
      MakeCountedFile<Cache::Strategy::FullCache>();

  TestSupport::BenchSessionGuard session;
  counted.file->PrefetchStanding(inside_offset, inside_length);
  counted.file->PrefetchStanding(across_offset, across_length);
  counted.file->CancelStanding(inside_offset, 1);
  counted.file->CancelStanding(across_offset, across_length);
  counted.file->CancelStanding(0, max_offset);
  const Bench::Report report = session.Finish();
  const Bench::ReadLog log = Bench::TakeReadLog();

  const std::vector<Call> announced =
      EventsOf(log, Bench::EventType::StandingPrefetch, Kind::PrefetchStanding);
  REQUIRE(announced.size() == 2);
  // The hint inside a block is one whole block, the other one two.
  CHECK(announced.at(0).offset == 3 * block_bytes);
  CHECK(announced.at(0).length == block_bytes);
  CHECK(announced.at(1).offset == 5 * block_bytes);
  CHECK(announced.at(1).length == 2 * block_bytes);
  const std::vector<Call> withdrawn =
      EventsOf(log, Bench::EventType::StandingCancel, Kind::CancelStanding);
  REQUIRE(withdrawn.size() == 3);
  CHECK(withdrawn.at(0).offset == 3 * block_bytes);
  CHECK(withdrawn.at(0).length == block_bytes);
  CHECK(withdrawn.at(1).offset == 5 * block_bytes);
  CHECK(withdrawn.at(1).length == 2 * block_bytes);
  CHECK(withdrawn.at(2).offset == 0);
  CHECK(withdrawn.at(2).length == max_offset);

  CHECK(report.standing_prefetch_calls == 2);
  CHECK(report.standing_prefetch_bytes == 3 * block_bytes);
  CHECK(report.standing_cancel_calls == 3);
  CheckLogMatchesReader(log, counted.counting->Calls());
}

TEST_CASE("A FileReader<FullCache> logs only the blocks the lane announces",
          "[bench][standing]") {
  CountedFile<Cache::Strategy::FullCache> counted =
      MakeCountedFile<Cache::Strategy::FullCache>();
  // The middle block of three is in the cache.
  LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(2 * block_bytes)};
  const auto held = counted.file->Read(addr, static_cast<DWORD>(block_bytes));
  REQUIRE(held.has_value());

  TestSupport::BenchSessionGuard session;
  counted.file->PrefetchStanding(block_bytes, 3 * block_bytes);
  const Bench::Report report = session.Finish();
  const Bench::ReadLog log = Bench::TakeReadLog();

  const std::vector<Call> announced =
      EventsOf(log, Bench::EventType::StandingPrefetch, Kind::PrefetchStanding);
  REQUIRE(announced.size() == 2);
  CHECK(announced.at(0).offset == block_bytes);
  CHECK(announced.at(0).length == block_bytes);
  CHECK(announced.at(1).offset == 3 * block_bytes);
  CHECK(announced.at(1).length == block_bytes);
  CHECK(report.standing_prefetch_calls == 2);
  CHECK(report.standing_prefetch_bytes == 2 * block_bytes);
}

TEMPLATE_TEST_CASE_SIG("The lane and the first queue are logged apart",
                       "[bench][standing]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CountedFile<S> counted = MakeCountedFile<S>();

  TestSupport::BenchSessionGuard session;
  counted.file->Prefetch(0, block_bytes);
  counted.file->PrefetchStanding(block_bytes, block_bytes);
  counted.file->CancelPrefetch();
  counted.file->CancelStanding(block_bytes, block_bytes);
  const Bench::Report report = session.Finish();
  const Bench::ReadLog log = Bench::TakeReadLog();

  CHECK(report.prefetch_calls == 1);
  CHECK(report.prefetch_bytes == block_bytes);
  CHECK(report.cancel_calls == 1);
  CHECK(report.standing_prefetch_calls == 1);
  CHECK(report.standing_prefetch_bytes == block_bytes);
  CHECK(report.standing_cancel_calls == 1);
  // The four calls are in the log in the order they were made.
  std::string types;
  for (const Bench::LogEvent& event : log.events) {
    types.push_back(static_cast<char>(event.type));
  }
  CHECK(types == "PSCX");
}

TEMPLATE_TEST_CASE_SIG(
    "A session over a volume logs the standing ranges of its $MFT slots",
    "[bench][standing]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  auto counting = std::make_unique<TestDiskReaders::CountingDiskReader>(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(
          TestFakeImage::BuildFakeNtfsImageWithFragmentedMftScan(450)));
  const TestDiskReaders::CountingDiskReader* const reader = counting.get();
  const NtfsVolume<S> volume(std::move(counting));
  REQUIRE(volume.IsVolumeOK());

  TestSupport::BenchSessionGuard session;
  // A range across the end of a fragment of $MFT: two ranges on the disk.
  volume.PrefetchMftSlots(90, 20);
  volume.PrefetchMftSlotsStanding(90, 20);
  volume.CancelMftSlotsStanding(90, 20);
  const Bench::Report report = session.Finish();
  const Bench::ReadLog log = Bench::TakeReadLog();

  const std::vector<Call> queued =
      EventsOf(log, Bench::EventType::Prefetch, Kind::PrefetchStanding);
  const std::vector<Call> announced =
      EventsOf(log, Bench::EventType::StandingPrefetch, Kind::PrefetchStanding);
  const std::vector<Call> withdrawn =
      EventsOf(log, Bench::EventType::StandingCancel, Kind::PrefetchStanding);
  REQUIRE_FALSE(queued.empty());
  // The lane names the ranges the first queue names, and withdraws them.
  CHECK(SameCalls(announced, queued));
  CHECK(SameCalls(withdrawn, queued));
  CHECK(report.standing_prefetch_calls == announced.size());
  CHECK(report.standing_cancel_calls == withdrawn.size());
  CHECK(report.prefetch_calls == queued.size());
  CheckLogMatchesReader(log, reader->Calls());
}

TEMPLATE_TEST_CASE_SIG(
    "Without a session the standing lane logs and counts nothing",
    "[bench][standing]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  CountedFile<S> counted = MakeCountedFile<S>();
  REQUIRE_FALSE(Bench::Enabled());
  // Whatever an earlier test left in the log of the last session.
  static_cast<void>(Bench::TakeReadLog());

  counted.file->PrefetchStanding(inside_offset, inside_length);
  counted.file->CancelStanding(inside_offset, inside_length);
  counted.file->CancelStanding(0, max_offset);

  CHECK_FALSE(Bench::Enabled());
  CHECK(Bench::TakeReadLog().events.empty());
  // The reader still got the calls.
  CHECK(CallsOf(counted.counting->Calls(), Kind::PrefetchStanding).size() == 1);
  CHECK(CallsOf(counted.counting->Calls(), Kind::CancelStanding).size() == 2);
}

TEST_CASE("A session that keeps no log counts the standing lane all the same",
          "[bench][standing]") {
  CountedFile<Cache::Strategy::NoCache> counted =
      MakeCountedFile<Cache::Strategy::NoCache>();

  TestSupport::BenchSessionGuard session(
      Bench::SessionOptions{.keep_log = false});
  counted.file->PrefetchStanding(inside_offset, inside_length);
  counted.file->CancelStanding(0, max_offset);
  const Bench::Report report = session.Finish();

  CHECK(report.standing_prefetch_calls == 1);
  CHECK(report.standing_prefetch_bytes == inside_length);
  CHECK(report.standing_cancel_calls == 1);
  CHECK(Bench::TakeReadLog().events.empty());
}
