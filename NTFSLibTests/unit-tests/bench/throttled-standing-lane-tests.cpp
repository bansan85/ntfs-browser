#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/disk-readers/throttled-disk-reader.h>
#include <ntfs-browser/tuning/tuning.h>

namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using Caps = std::array<Tuning::ReaderTuning, Tuning::pattern_count>;

namespace {

// One MiB and four KiB, the sizes the ranges below are made of.
constexpr ULONGLONG mib = std::uint64_t{1} << 20U;
constexpr ULONGLONG page = 4096;

// The largest offset: an interval that runs to it withdraws everything.
constexpr ULONGLONG max_offset = (std::numeric_limits<ULONGLONG>::max)();

// Size of the buffer behind the readers: every offset the tests read is in it.
constexpr std::size_t disk_bytes = std::size_t{16} << 20U;

// A model with round numbers, and many lanes so that the reads of a test do
// not wait for one another.
Tests::DiskModel RoundModel() {
  Tests::DiskModel model;
  model.seek_ns = 1000;
  model.per_read_ns = 500;
  model.single_bytes_per_second = 1'000'000'000;
  model.max_bytes_per_second = 2'000'000'000;
  model.lanes = 64;
  model.consume_ns_per_mib = 0;
  return model;
}

std::unique_ptr<Tests::ThrottledDiskReader>
    MakeReader(Tests::SimulatedClock& clock, const Caps& caps = {}) {
  return std::make_unique<Tests::ThrottledDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(
          std::vector<BYTE>(disk_bytes, BYTE{7})),
      RoundModel(), clock, caps);
}

// The caps of every pattern, with one field set by set.
template <typename Set>
Caps CapsWith(const Set& set) {
  Caps caps;
  for (Tuning::ReaderTuning& tuning : caps) {
    set(tuning);
  }
  return caps;
}

// Reads length bytes at offset, which the test cannot go on without.
void Read(const Tests::ThrottledDiskReader& reader, ULONGLONG offset,
          ULONGLONG length) {
  std::vector<BYTE> buffer(static_cast<std::size_t>(length));
  LARGE_INTEGER addr{};
  addr.QuadPart = static_cast<LONGLONG>(offset);
  REQUIRE(reader.ReadInto(addr, buffer));
}

// What a scripted run left behind.
struct Outcome {
  std::uint64_t now_ns;
  Tests::ThrottleStats stats;
};

// Announces three ranges and reads them, through announce, which is
// Prefetch() or PrefetchStanding().
template <typename Announce>
Outcome RunScript(const Announce& announce) {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock);
  announce(*reader, 0, 64 * page);
  announce(*reader, mib, 16 * page);
  announce(*reader, 5 * mib, page);
  Read(*reader, 0, 64 * page);
  Read(*reader, 3 * mib, 8 * page);
  clock.Advance(123456);
  Read(*reader, mib, 16 * page);
  Read(*reader, 5 * mib, page);
  return {.now_ns = clock.NowNs(), .stats = reader->Stats()};
}

}  // namespace

TEST_CASE("The standing lane costs the time that the first queue costs",
          "[bench][throttled][standing]") {
  const Outcome first =
      RunScript([](const Tests::ThrottledDiskReader& reader, ULONGLONG offset,
                   ULONGLONG length) { reader.Prefetch(offset, length); });
  const Outcome lane = RunScript(
      [](const Tests::ThrottledDiskReader& reader, ULONGLONG offset,
         ULONGLONG length) { reader.PrefetchStanding(offset, length); });

  CHECK(lane.now_ns == first.now_ns);
  CHECK(lane.stats == first.stats);
  CHECK(lane.stats.served_reads == 3);
  CHECK(lane.stats.unannounced_reads == 1);
}

TEST_CASE("The same standing announcements and reads give the same time",
          "[bench][throttled][standing]") {
  const auto announce = [](const Tests::ThrottledDiskReader& reader,
                           ULONGLONG offset, ULONGLONG length) {
    reader.PrefetchStanding(offset, length);
  };

  const Outcome first = RunScript(announce);
  const Outcome second = RunScript(announce);

  CHECK(first.now_ns > 0);
  CHECK(first.now_ns == second.now_ns);
  CHECK(first.stats == second.stats);
}

TEST_CASE("A range announced early on the lane costs less than no hint",
          "[bench][throttled][standing]") {
  Tests::SimulatedClock hinted_clock;
  const auto hinted = MakeReader(hinted_clock);
  Tests::SimulatedClock plain_clock;
  const auto plain = MakeReader(plain_clock);

  hinted->PrefetchStanding(mib, mib);
  // The caller works elsewhere while the lane reads.
  const std::uint64_t work_ns = 5000000;
  hinted_clock.Advance(work_ns);
  plain_clock.Advance(work_ns);
  Read(*hinted, mib, mib);
  Read(*plain, mib, mib);

  CHECK(hinted->Stats().wait_ns == 0);
  CHECK(plain->Stats().wait_ns > 0);
  CHECK(hinted_clock.NowNs() < plain_clock.NowNs());
  CHECK(hinted->Stats().served_reads == 1);
  CHECK(plain->Stats().unannounced_reads == 1);
}

TEST_CASE("A lane that nobody reads leaves the time of the reads as it was",
          "[bench][throttled][standing]") {
  Tests::SimulatedClock clock;
  const auto idle = MakeReader(clock);
  Tests::SimulatedClock lane_clock;
  const auto lane = MakeReader(lane_clock);
  for (std::size_t index = 0; index < 3; index++) {
    lane->PrefetchStanding((index + 8) * mib, page);
  }

  // Once the reads of the lane are over, the disk is free again.
  const std::uint64_t later = 1'000'000'000;
  clock.Advance(later);
  lane_clock.Advance(later);
  Read(*idle, 0, 8 * page);
  Read(*lane, 0, 8 * page);

  CHECK(lane->Stats().wait_ns == idle->Stats().wait_ns);
  CHECK(lane_clock.NowNs() == clock.NowNs());
  CHECK(lane->Stats().disk_reads == idle->Stats().disk_reads + 3);
  lane->CancelStanding(0, max_offset);
  CHECK(lane->Stats().wasted_ranges == 3);
}

TEST_CASE("CancelPrefetch leaves the standing lane alone",
          "[bench][throttled][standing]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock);
  reader->PrefetchStanding(0, mib);
  reader->Prefetch(5 * mib, page);

  reader->CancelPrefetch();

  // Only the range of the first queue was withdrawn.
  CHECK(reader->Stats().wasted_ranges == 1);
  Read(*reader, 0, mib);
  CHECK(reader->Stats().served_reads == 1);
  CHECK(reader->Stats().unannounced_reads == 0);
  CHECK(reader->Stats().disk_reads == 2);
}

TEST_CASE("A read of either queue drops no range of the lane",
          "[bench][throttled][standing]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock);
  reader->PrefetchStanding(0, page);
  reader->PrefetchStanding(mib, page);
  reader->PrefetchStanding(2 * mib, page);
  reader->Prefetch(3 * mib, page);

  // The first queue, then the lane out of its order.
  Read(*reader, 3 * mib, page);
  Read(*reader, 2 * mib, page);
  Read(*reader, 0, page);
  Read(*reader, mib, page);

  CHECK(reader->Stats().served_reads == 4);
  CHECK(reader->Stats().unannounced_reads == 0);
  CHECK(reader->Stats().wasted_ranges == 0);
  CHECK(reader->Stats().disk_reads == 4);
}

TEST_CASE("The first queue is tried before the lane",
          "[bench][throttled][standing]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock);
  // The lane holds a wider range than the first queue, and the same bytes.
  reader->PrefetchStanding(0, 4 * page);
  reader->Prefetch(page, page);

  Read(*reader, page, page);

  // The first queue served it, and used its range up: the lane keeps its own.
  CHECK(reader->Stats().served_reads == 1);
  Read(*reader, 3 * page, page);
  CHECK(reader->Stats().served_reads == 2);
  CHECK(reader->Stats().disk_reads == 2);
  CHECK(reader->Stats().wasted_ranges == 0);
}

TEST_CASE("CancelStanding withdraws the ranges that overlap the interval",
          "[bench][throttled][standing]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock);
  for (std::size_t index = 0; index < 4; index++) {
    reader->PrefetchStanding(index * mib, 2 * page);
  }

  SECTION("a length of 0 overlaps nothing") {
    reader->CancelStanding(mib, 0);
    CHECK(reader->Stats().wasted_ranges == 0);
  }

  SECTION("an interval inside one range withdraws that range only") {
    reader->CancelStanding(mib + page, 1);
    CHECK(reader->Stats().wasted_ranges == 1);
    Read(*reader, mib, page);
    CHECK(reader->Stats().unannounced_reads == 1);
    Read(*reader, 0, page);
    Read(*reader, 2 * mib, page);
    CHECK(reader->Stats().served_reads == 2);
  }

  SECTION("an interval that ends where a range starts overlaps nothing") {
    reader->CancelStanding(mib - page, page);
    CHECK(reader->Stats().wasted_ranges == 0);
  }

  SECTION("an interval over several ranges withdraws each") {
    reader->CancelStanding(mib + page, 2 * mib);
    CHECK(reader->Stats().wasted_ranges == 3);
  }

  SECTION("an interval that runs to the largest offset withdraws the rest") {
    reader->CancelStanding(2 * mib, max_offset);
    CHECK(reader->Stats().wasted_ranges == 2);
    reader->CancelStanding(0, max_offset);
    CHECK(reader->Stats().wasted_ranges == 4);
    // Nothing is left to withdraw.
    reader->CancelStanding(0, max_offset);
    CHECK(reader->Stats().wasted_ranges == 4);
  }

  SECTION("a range that was read is not counted as wasted") {
    Read(*reader, 0, 2 * page);
    reader->CancelStanding(0, max_offset);
    CHECK(reader->Stats().wasted_ranges == 3);
  }
}

TEST_CASE("A range of the first queue equal to a lane range is ignored",
          "[bench][throttled][standing]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock);
  reader->PrefetchStanding(mib, 4 * page);

  reader->Prefetch(mib, 4 * page);
  CHECK(reader->Stats().disk_reads == 1);

  // A range that only overlaps is another range.
  reader->Prefetch(mib, 2 * page);
  CHECK(reader->Stats().disk_reads == 2);

  Read(*reader, mib, 4 * page);
  CHECK(reader->Stats().served_reads == 1);
}

TEST_CASE("The lane leaves a share of the read slots to the first queue",
          "[bench][throttled][standing]") {
  // A quarter of the slots, and at least one, stay with the first queue.
  const auto row = GENERATE(table<std::size_t, std::size_t>(
      {{1, 0}, {2, 1}, {3, 2}, {4, 3}, {8, 6}, {32, 24}}));
  const std::size_t max_reads = std::get<0>(row);
  const std::size_t lane_reads = std::get<1>(row);
  INFO("max_reads " << max_reads);
  const Caps caps = CapsWith([max_reads](Tuning::ReaderTuning& tuning) {
    tuning.max_reads = max_reads;
  });
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);

  for (std::size_t index = 0; index < 40; index++) {
    reader->PrefetchStanding(index * 2 * page, page);
  }
  CHECK(reader->Stats().disk_reads == lane_reads);

  // The first queue takes the rest, whatever the lane holds.
  for (std::size_t index = 0; index < 40; index++) {
    reader->Prefetch(mib + index * 2 * page, page);
  }
  CHECK(reader->Stats().disk_reads == max_reads);
}

TEST_CASE("The first queue starts before the lane in the freed reads",
          "[bench][throttled][standing]") {
  const Caps caps =
      CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_reads = 4; });
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);
  for (std::size_t index = 0; index < 6; index++) {
    reader->PrefetchStanding(index * 2 * page, page);
  }
  for (std::size_t index = 0; index < 3; index++) {
    reader->Prefetch(mib + index * 2 * page, page);
  }
  // The lane has 3 reads in flight, the first queue the fourth.
  REQUIRE(reader->Stats().disk_reads == 4);

  // A read of the lane is withdrawn: the first queue, which comes first, gets
  // the freed read, and so it does for the next one.
  reader->CancelStanding(0, page);
  CHECK(reader->Stats().disk_reads == 5);
  reader->CancelStanding(2 * page, page);
  CHECK(reader->Stats().disk_reads == 6);
  // The lane has one read in flight and the first queue three: no read is
  // free for the lane, whose ranges are still waiting.
  reader->CancelStanding(10 * page, page);
  CHECK(reader->Stats().disk_reads == 6);
}

TEST_CASE("A finished read of the lane holds no read in flight",
          "[bench][throttled][standing]") {
  const Caps caps =
      CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_reads = 4; });
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);
  for (std::size_t index = 0; index < 8; index++) {
    reader->PrefetchStanding(index * 2 * page, page);
  }
  // The lane keeps 3 reads in flight: the first queue keeps the fourth.
  CHECK(reader->Stats().disk_reads == 3);

  // The three reads finish, and wait for their caller: the lane starts the
  // next three, then the last two. Nothing was read by the caller.
  clock.Advance(mib);
  reader->PrefetchStanding(100 * page, page);
  CHECK(reader->Stats().disk_reads == 6);
  clock.Advance(mib);
  reader->PrefetchStanding(102 * page, page);
  CHECK(reader->Stats().disk_reads == 9);
  CHECK(reader->Stats().served_reads == 0);
}

TEST_CASE("The first queue's slots go to the lane when it is withdrawn",
          "[bench][throttled][standing]") {
  const Caps caps =
      CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_reads = 4; });
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);
  for (std::size_t index = 0; index < 6; index++) {
    reader->PrefetchStanding(index * 2 * page, page);
  }
  for (std::size_t index = 0; index < 3; index++) {
    reader->Prefetch(mib + index * 2 * page, page);
  }
  REQUIRE(reader->Stats().disk_reads == 4);

  reader->CancelPrefetch();

  // The lane holds three slots at most, so nothing more starts.
  CHECK(reader->Stats().disk_reads == 4);
  reader->CancelStanding(0, page);
  // One lane read is withdrawn, and the next range of the lane starts.
  CHECK(reader->Stats().disk_reads == 5);
}

TEST_CASE("The lane and the first queue share the bytes in flight",
          "[bench][throttled][standing]") {
  const Caps caps = CapsWith(
      [](Tuning::ReaderTuning& tuning) { tuning.max_bytes = 4 * page; });
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);

  // The first one always starts, then the ones that fit.
  reader->Prefetch(mib, 2 * page);
  reader->PrefetchStanding(0, 2 * page);
  reader->PrefetchStanding(2 * mib, 2 * page);
  reader->PrefetchStanding(3 * mib, page);

  // 2 + 2 pages fit the cap. The third range of the lane does not, and the
  // fourth waits behind it: the lane starts its reads in order.
  CHECK(reader->Stats().disk_reads == 2);
  Read(*reader, 0, 2 * page);
  CHECK(reader->Stats().disk_reads == 3);
}

TEST_CASE("A lone range over the byte cap still starts on the lane",
          "[bench][throttled][standing]") {
  const Caps caps =
      CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_bytes = page; });
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);

  reader->PrefetchStanding(0, 8 * page);

  CHECK(reader->Stats().disk_reads == 1);
  Read(*reader, 0, 8 * page);
  CHECK(reader->Stats().served_reads == 1);
}

TEST_CASE("The lane holds at most max_announced_ranges ranges",
          "[bench][throttled][standing]") {
  const Caps caps = CapsWith(
      [](Tuning::ReaderTuning& tuning) { tuning.max_announced_ranges = 3; });
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);

  for (std::size_t index = 0; index < 5; index++) {
    reader->PrefetchStanding(index * mib, page);
  }
  // The first queue has a cap of its own.
  reader->Prefetch(8 * mib, page);

  CHECK(reader->Stats().disk_reads == 4);
  Read(*reader, 4 * mib, page);
  CHECK(reader->Stats().unannounced_reads == 1);
  Read(*reader, 2 * mib, page);
  CHECK(reader->Stats().served_reads == 1);
}

TEST_CASE("A range of the lane over the range cap is not announced",
          "[bench][throttled][standing]") {
  const Caps caps = CapsWith(
      [](Tuning::ReaderTuning& tuning) { tuning.max_range_bytes = 8 * page; });
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);

  reader->PrefetchStanding(0, 64 * page);
  reader->PrefetchStanding(mib, 0);
  reader->PrefetchStanding(max_offset - page, 2 * page);

  CHECK(reader->Stats().disk_reads == 0);
  Read(*reader, 0, 64 * page);
  CHECK(reader->Stats().unannounced_reads == 1);
}

TEST_CASE("A range of the lane that never started is not served",
          "[bench][throttled][standing]") {
  const Caps caps =
      CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_reads = 4; });
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);
  for (std::size_t index = 0; index < 4; index++) {
    reader->PrefetchStanding(index * mib, page);
  }
  REQUIRE(reader->Stats().disk_reads == 3);

  // The fourth range is held back by the caps: the caller reads on its own,
  // and the range is gone.
  Read(*reader, 3 * mib, page);
  CHECK(reader->Stats().unannounced_reads == 1);
  CHECK(reader->Stats().served_reads == 0);
  CHECK(reader->Stats().wasted_ranges == 0);
  reader->CancelStanding(0, max_offset);
  CHECK(reader->Stats().wasted_ranges == 3);
}

TEST_CASE("The lane starts under the caps of the pattern in force",
          "[bench][throttled][standing]") {
  Caps caps;
  caps.at(static_cast<std::size_t>(Tuning::Pattern::ScatteredGrouped))
      .max_reads = 1;
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(clock, caps);

  reader->SetReadPattern(Tuning::Pattern::ScatteredGrouped);
  for (std::size_t index = 0; index < 3; index++) {
    reader->PrefetchStanding(index * mib, page);
  }
  // One read in all, and the first queue keeps it.
  CHECK(reader->Stats().disk_reads == 0);

  reader->SetReadPattern(Tuning::Pattern::LargeSequential);
  CHECK(reader->Stats().disk_reads == 3);
}

TEST_CASE("The wrapped reader gives the bytes whatever the lane does",
          "[bench][throttled][standing]") {
  std::vector<BYTE> image(disk_bytes);
  for (std::size_t index = 0; index < image.size(); index++) {
    image.at(index) = static_cast<BYTE>((index * 7) + (index >> 9U));
  }
  Tests::SimulatedClock clock;
  const Tests::ThrottledDiskReader reader(
      std::make_unique<Tests::MemoryDiskReader>(image), RoundModel(), clock);
  reader.PrefetchStanding(mib, 4 * page);

  std::vector<BYTE> served(2 * page);
  LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(mib + page)};
  REQUIRE(reader.ReadInto(addr, served));
  std::vector<BYTE> plain(2 * page);
  addr.QuadPart = static_cast<LONGLONG>(8 * mib);
  REQUIRE(reader.ReadInto(addr, plain));

  const auto expected = [&image](ULONGLONG offset) {
    return std::vector<BYTE>(
        image.begin() + static_cast<std::ptrdiff_t>(offset),
        image.begin() + static_cast<std::ptrdiff_t>(offset + 2 * page));
  };
  CHECK(served == expected(mib + page));
  CHECK(plain == expected(8 * mib));
}
