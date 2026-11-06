#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/knobs.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "throttled-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Caps = std::array<Tuning::ReaderTuning, Tuning::pattern_count>;

namespace {

// One MiB and four KiB, the sizes the arithmetic below is done in.
constexpr ULONGLONG mib = std::uint64_t{1} << 20U;
constexpr ULONGLONG page = 4096;

// Size of the buffer behind the readers: every offset the tests read is in it.
constexpr std::size_t disk_bytes = std::size_t{16} << 20U;

// Nanoseconds a 1 MiB transfer takes at 100 MB/s: 1048576 B * 1e9 / 1e8.
constexpr std::uint64_t mib_at_100_mbps_ns = 10'485'760;

// A model with round numbers: one byte takes one nanosecond for a single read,
// half of one for the whole disk.
Tests::DiskModel RoundModel() {
  Tests::DiskModel model;
  model.seek_ns = 1000;
  model.per_read_ns = 500;
  model.single_bytes_per_second = 1'000'000'000;
  model.max_bytes_per_second = 2'000'000'000;
  model.lanes = 2;
  model.consume_ns_per_mib = 0;
  return model;
}

// The simulated hard disk of NtfsBench: a long seek, one lane.
Tests::DiskModel HardDisk() {
  Tests::DiskModel model;
  model.seek_ns = 8000000;
  model.per_read_ns = 100000;
  model.single_bytes_per_second = 116'000'000;
  model.max_bytes_per_second = 116'000'000;
  model.lanes = 1;
  return model;
}

std::unique_ptr<Tests::ThrottledDiskReader>
    MakeReader(const Tests::DiskModel& model, Tests::SimulatedClock& clock,
               const Caps& caps = {}) {
  return std::make_unique<Tests::ThrottledDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(
          std::vector<BYTE>(disk_bytes, BYTE{7})),
      model, clock, caps);
}

// Reads length bytes at offset, which the test cannot go on without.
void Read(const Tests::ThrottledDiskReader& reader, ULONGLONG offset,
          ULONGLONG length) {
  std::vector<BYTE> buffer(static_cast<std::size_t>(length));
  LARGE_INTEGER addr{};
  addr.QuadPart = static_cast<LONGLONG>(offset);
  REQUIRE(reader.ReadInto(addr, buffer));
}

// Announces count adjacent ranges of one MiB from offset 0, then reads them
// all, in order.
void ReadAnnouncedMebibytes(const Tests::ThrottledDiskReader& reader,
                            std::size_t count) {
  for (std::size_t index = 0; index < count; index++) {
    reader.Prefetch(index * mib, mib);
  }
  for (std::size_t index = 0; index < count; index++) {
    Read(reader, index * mib, mib);
  }
}

// What a run of the scripted reads left behind.
struct Outcome {
  std::uint64_t now_ns;
  Tests::ThrottleStats stats;
};

bool SameStats(const Tests::ThrottleStats& lhs,
               const Tests::ThrottleStats& rhs) {
  return lhs.disk_reads == rhs.disk_reads && lhs.disk_bytes == rhs.disk_bytes &&
         lhs.served_reads == rhs.served_reads &&
         lhs.unannounced_reads == rhs.unannounced_reads &&
         lhs.wait_ns == rhs.wait_ns && lhs.wasted_ranges == rhs.wasted_ranges;
}

// Announcements, reads of both kinds, a withdrawal and a pattern change.
Outcome RunScript() {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock);
  reader->Prefetch(0, 64 * page);
  reader->Prefetch(mib, 16 * page);
  reader->Prefetch(5 * mib, page);
  Read(*reader, 0, 64 * page);
  Read(*reader, 3 * mib, 8 * page);
  reader->SetReadPattern(Tuning::Pattern::ScatteredGrouped);
  Read(*reader, mib, 16 * page);
  reader->CancelPrefetch();
  reader->Prefetch(8 * mib, 32 * page);
  clock.Advance(123456);
  Read(*reader, 8 * mib, 32 * page);
  return {.now_ns = clock.NowNs(), .stats = reader->Stats()};
}

// What a scan of the list fixture costs on a simulated disk.
struct ScanOutcome {
  std::uint64_t sim_ns;
  std::size_t entries;
  Tests::ThrottleStats stats;
};

// Slots of the fixture whose $MFT the scan reads, and slots per fragment.
constexpr ULONGLONG scan_records = 4000;
constexpr ULONGLONG scan_fragment_clusters = 250;

// CPU time the scan spends on each MiB it reads, so that parsing overlaps the
// reads ahead: the one NtfsBench simulate uses.
constexpr std::uint64_t scan_consume_ns_per_mib = 1000000;

ScanOutcome ScanOnDisk(const std::vector<BYTE>& image, Tests::DiskModel model,
                       const VolumeOptions& options) {
  model.consume_ns_per_mib = scan_consume_ns_per_mib;
  Tests::SimulatedClock clock;
  auto throttled = std::make_unique<Tests::ThrottledDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(image), model, clock,
      options.tuning.reader);
  const Tests::ThrottledDiskReader* const disk = throttled.get();
  const NtfsVolume<Cache::Strategy::NoCache> volume(std::move(throttled),
                                                    options);
  REQUIRE(volume.IsVolumeOK());
  Mft::MftTree::ScanOptions scan;
  scan.threads = 1;
  const Mft::MftTree tree(volume, scan);
  REQUIRE(tree.Stats().complete);
  return {.sim_ns = clock.NowNs(),
          .entries = tree.Entries().size(),
          .stats = disk->Stats()};
}

VolumeOptions WithKnob(std::string_view name, std::uint64_t value) {
  VolumeOptions options;
  const std::string failure = Tuning::ApplyKnob(options, name, value);
  REQUIRE(failure.empty());
  return options;
}

}  // namespace

TEST_CASE("A simulated read costs what the model's arithmetic says",
          "[bench][throttled]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock);

  // The first read seeks: 500 + 1000 + 4096 bytes at one byte per ns.
  Read(*reader, 2 * page, page);
  CHECK(clock.NowNs() == 5596);
  CHECK(reader->Stats().wait_ns == 5596);

  // The next read starts where the last one ended: no seek. The second lane
  // is free, so it starts at once, after the clock.
  Read(*reader, 3 * page, page);
  CHECK(clock.NowNs() == 5596 + 500 + 4096);

  // A read that does not follow the previous one seeks again.
  Read(*reader, mib, page);
  CHECK(clock.NowNs() == 5596 + 500 + 4096 + 500 + 1000 + 4096);

  const Tests::ThrottleStats& stats = reader->Stats();
  CHECK(stats.disk_reads == 3);
  CHECK(stats.disk_bytes == 3 * page);
  CHECK(stats.unannounced_reads == 3);
  CHECK(stats.served_reads == 0);
  CHECK(stats.wait_ns == clock.NowNs());
  CHECK(stats.wasted_ranges == 0);
}

TEST_CASE("The consumer's work is priced per MiB handed over",
          "[bench][throttled]") {
  Tests::DiskModel model = RoundModel();
  model.consume_ns_per_mib = 1000000;
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(model, clock);

  Read(*reader, 0, mib);

  const std::uint64_t read_ns = 500 + 1000 + mib;
  CHECK(clock.NowNs() == read_ns + 1000000);
  CHECK(reader->Stats().wait_ns == read_ns);
}

TEST_CASE("The same reads give the same simulated time", "[bench][throttled]") {
  const Outcome first = RunScript();
  const Outcome second = RunScript();

  CHECK(first.now_ns > 0);
  CHECK(first.now_ns == second.now_ns);
  CHECK(SameStats(first.stats, second.stats));
  CHECK(first.stats.served_reads > 0);
  CHECK(first.stats.unannounced_reads > 0);
}

TEST_CASE("A long simulated run takes no real time", "[bench][throttled]") {
  // 1 MB/s: each MiB read takes about a second of simulated time.
  Tests::DiskModel model = RoundModel();
  model.single_bytes_per_second = 1000000;
  model.max_bytes_per_second = 1000000;
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(model, clock);
  constexpr std::size_t reads = 1200;

  const auto began = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < reads; index++) {
    Read(*reader, 0, mib);
  }
  const auto real = std::chrono::steady_clock::now() - began;

  constexpr std::uint64_t one_second_ns = 1'000'000'000;
  CHECK(clock.NowNs() > 1000 * one_second_ns);
  CHECK(real < std::chrono::seconds(30));
}

TEST_CASE("A read that matches an announced range waits until it completes",
          "[bench][throttled]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock);

  reader->Prefetch(0, mib);
  // Announcing starts the read; it costs the caller nothing.
  CHECK(clock.NowNs() == 0);
  CHECK(reader->Stats().disk_reads == 1);

  Read(*reader, 0, mib);

  const std::uint64_t done = 500 + 1000 + mib;
  CHECK(clock.NowNs() == done);
  CHECK(reader->Stats().wait_ns == done);
  CHECK(reader->Stats().served_reads == 1);
  CHECK(reader->Stats().unannounced_reads == 0);
  CHECK(reader->Stats().disk_reads == 1);
}

TEST_CASE("A range already complete costs the caller no wait",
          "[bench][throttled]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock);
  reader->Prefetch(0, mib);
  const std::uint64_t late = 5000000;
  clock.Advance(late);

  Read(*reader, 0, mib);

  CHECK(clock.NowNs() == late);
  CHECK(reader->Stats().wait_ns == 0);
  CHECK(reader->Stats().served_reads == 1);
}

TEST_CASE("A read in the middle of a range is served and keeps the range",
          "[bench][throttled]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock);
  reader->Prefetch(0, 4 * page);

  Read(*reader, page, page);
  Read(*reader, 3 * page, page);

  CHECK(reader->Stats().served_reads == 2);
  CHECK(reader->Stats().disk_reads == 1);
  // The second read reached the end of the range: it is used up, not wasted.
  reader->CancelPrefetch();
  CHECK(reader->Stats().wasted_ranges == 0);
}

TEST_CASE("A read outside every announced range is synchronous",
          "[bench][throttled]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock);
  reader->Prefetch(0, page);

  // Across the end of the range: not inside it.
  Read(*reader, 0, 2 * page);
  Read(*reader, 4 * mib, page);

  const Tests::ThrottleStats& stats = reader->Stats();
  CHECK(stats.unannounced_reads == 2);
  CHECK(stats.served_reads == 0);
  // The announced range and the two reads.
  CHECK(stats.disk_reads == 3);
  CHECK(stats.wait_ns > 0);
  CHECK(stats.wait_ns == clock.NowNs());
}

TEST_CASE("A read that matches a later range drops the ones before it",
          "[bench][throttled]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock);
  reader->Prefetch(0, page);
  reader->Prefetch(mib, page);
  reader->Prefetch(2 * mib, page);

  Read(*reader, 2 * mib, page);

  CHECK(reader->Stats().served_reads == 1);
  CHECK(reader->Stats().wasted_ranges == 2);
}

TEST_CASE("Withdrawing the announcements counts the ranges that were wasted",
          "[bench][throttled]") {
  const std::size_t max_reads = GENERATE(1U, 2U, 32U);
  INFO("max_reads " << max_reads);
  Caps caps;
  for (Tuning::ReaderTuning& tuning : caps) {
    tuning.max_reads = max_reads;
  }
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock, caps);
  for (std::size_t index = 0; index < 3; index++) {
    reader->Prefetch(index * mib, page);
  }

  reader->CancelPrefetch();

  // A range that never started was never read: nothing was wasted on it.
  CHECK(reader->Stats().wasted_ranges == (std::min<std::size_t>)(max_reads, 3));
  CHECK(reader->Stats().disk_reads == (std::min<std::size_t>)(max_reads, 3));

  // Withdrawing again changes nothing.
  reader->CancelPrefetch();
  CHECK(reader->Stats().wasted_ranges == (std::min<std::size_t>)(max_reads, 3));
}

TEST_CASE("A range a read used is not counted as wasted",
          "[bench][throttled]") {
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock);
  reader->Prefetch(0, 4 * page);
  reader->Prefetch(mib, page);

  // Part of the first range: used, but not finished.
  Read(*reader, 0, page);
  reader->CancelPrefetch();

  CHECK(reader->Stats().wasted_ranges == 1);
}

TEST_CASE("A hint over the range cap is not announced", "[bench][throttled]") {
  Caps caps;
  for (Tuning::ReaderTuning& tuning : caps) {
    tuning.max_range_bytes = 8 * page;
  }
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock, caps);

  reader->Prefetch(0, 64 * page);
  reader->Prefetch(mib, 0);

  CHECK(reader->Stats().disk_reads == 0);
  Read(*reader, 0, 64 * page);
  CHECK(reader->Stats().unannounced_reads == 1);
  CHECK(reader->Stats().served_reads == 0);
}

TEST_CASE("The caps of the pattern in force decide how many reads start",
          "[bench][throttled]") {
  Caps caps;
  caps.at(static_cast<std::size_t>(Tuning::Pattern::ScatteredGrouped))
      .max_reads = 1;
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock, caps);
  const auto announce = [&reader] {
    for (std::size_t index = 0; index < 3; index++) {
      reader->Prefetch(index * mib, page);
    }
  };

  reader->SetReadPattern(Tuning::Pattern::ScatteredGrouped);
  announce();
  CHECK(reader->Stats().disk_reads == 1);

  // Back to a pattern that allows more: the waiting ranges start.
  reader->SetReadPattern(Tuning::Pattern::LargeSequential);
  CHECK(reader->Stats().disk_reads == 3);
}

TEST_CASE("The bytes in flight are capped by max_bytes", "[bench][throttled]") {
  const std::uint64_t max_bytes = GENERATE(page, 4 * page, 64 * mib);
  INFO("max_bytes " << max_bytes);
  Caps caps;
  for (Tuning::ReaderTuning& tuning : caps) {
    tuning.max_bytes = max_bytes;
  }
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(RoundModel(), clock, caps);

  for (std::size_t index = 0; index < 8; index++) {
    reader->Prefetch(index * mib, 2 * page);
  }

  // The first range always starts, then as many as the cap holds.
  const std::uint64_t expected =
      (std::clamp<std::uint64_t>)(max_bytes / (2 * page), 1, 8);
  CHECK(reader->Stats().disk_reads == expected);
}

TEST_CASE("The lanes of the model bound how many reads overlap",
          "[bench][throttled]") {
  const std::uint32_t lanes = GENERATE(1U, 2U, 4U);
  INFO("lanes " << lanes);
  // Four reads of one MiB at 100 MB/s each, with a disk that could carry far
  // more: only the lanes limit the overlap.
  Tests::DiskModel model;
  model.seek_ns = 0;
  model.per_read_ns = 0;
  model.single_bytes_per_second = 100'000'000;
  model.max_bytes_per_second = 1'000'000'000'000;
  model.lanes = lanes;
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(model, clock);

  ReadAnnouncedMebibytes(*reader, 4);

  const std::uint64_t rounds = (4 + lanes - 1) / lanes;
  CHECK(clock.NowNs() == rounds * mib_at_100_mbps_ns);
}

TEST_CASE("The bandwidth of the model bounds the throughput",
          "[bench][throttled]") {
  const std::size_t count = GENERATE(1U, 4U, 8U);
  INFO("ranges " << count);
  // Eight lanes at 1 GB/s each, but 100 MB/s for the disk as a whole.
  Tests::DiskModel model;
  model.seek_ns = 0;
  model.per_read_ns = 0;
  model.single_bytes_per_second = 1'000'000'000;
  model.max_bytes_per_second = 100'000'000;
  model.lanes = 8;
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(model, clock);

  ReadAnnouncedMebibytes(*reader, count);

  CHECK(clock.NowNs() == count * mib_at_100_mbps_ns);
  const double seconds = static_cast<double>(clock.NowNs()) / 1e9;
  const double rate = static_cast<double>(reader->Stats().disk_bytes) / seconds;
  CHECK(rate <= 100'000'000.0 * 1.000001);
}

TEST_CASE("A single read is bound by the speed of one lane",
          "[bench][throttled]") {
  // The disk could carry 2 GB/s, but one read streams at 100 MB/s.
  Tests::DiskModel model;
  model.seek_ns = 0;
  model.per_read_ns = 0;
  model.single_bytes_per_second = 100'000'000;
  model.max_bytes_per_second = 2'000'000'000;
  model.lanes = 8;
  Tests::SimulatedClock clock;
  const auto reader = MakeReader(model, clock);

  Read(*reader, 0, mib);

  CHECK(clock.NowNs() == mib_at_100_mbps_ns);
}

TEST_CASE("A scan with fewer reads in flight takes longer",
          "[bench][throttled]") {
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithListMftScan(
      scan_records, scan_fragment_clusters);
  const bool hard_disk = GENERATE(false, true);
  const std::string knob =
      GENERATE(as<std::string>{}, "reader.max_reads", "reader.max_bytes");
  INFO((hard_disk ? "hard disk, " : "SSD, ") << knob);
  const Tests::DiskModel model = hard_disk ? HardDisk() : Tests::DiskModel{};
  const std::uint64_t smallest = knob == "reader.max_reads" ? 1 : page;

  const ScanOutcome normal = ScanOnDisk(image, model, VolumeOptions{});
  const ScanOutcome starved =
      ScanOnDisk(image, model, WithKnob(knob, smallest));

  CHECK(starved.sim_ns > normal.sim_ns);
  // The caps change when the bytes are read, never what the scan finds.
  CHECK(starved.entries == normal.entries);
}

TEST_CASE("A scan on a simulated disk is repeatable and serves its hints",
          "[bench][throttled]") {
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithListMftScan(
      scan_records, scan_fragment_clusters);
  const bool hard_disk = GENERATE(false, true);
  const Tests::DiskModel model = hard_disk ? HardDisk() : Tests::DiskModel{};

  const ScanOutcome first = ScanOnDisk(image, model, VolumeOptions{});
  const ScanOutcome second = ScanOnDisk(image, model, VolumeOptions{});

  CHECK(first.sim_ns > 0);
  CHECK(first.sim_ns == second.sim_ns);
  CHECK(SameStats(first.stats, second.stats));
  CHECK(first.stats.served_reads > 0);
  CHECK(first.entries > 0);
  // A hard disk with one lane and a long seek is slower than the SSD.
  if (hard_disk) {
    const ScanOutcome ssd =
        ScanOnDisk(image, Tests::DiskModel{}, VolumeOptions{});
    CHECK(first.sim_ns > ssd.sim_ns);
  }
}
