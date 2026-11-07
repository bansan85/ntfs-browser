#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/mft/mft-tree-state-support.h>
#include <ntfs-browser/test/support/bench-session-guard.h>
#include <ntfs-browser/test/support/scan-comparison.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "alloc/cluster-bitmap.h"
#include "knob-effect-support.h"
#include "mft/tree-testing.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace TestMft = NTFS_BROWSER_NAMESPACE::Test::Mft;
namespace TestTuning = NTFS_BROWSER_NAMESPACE::Test::Tuning;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestAlloc = NTFS_BROWSER_NAMESPACE::Test::Alloc;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using TestSupport::ScanResult;
using namespace TestTuning;

namespace {

// The list fixture of the threads tests: 3000 slots, and a cluster of its own
// for the list of some files.
constexpr ULONGLONG list_record_count = 3000;
constexpr ULONGLONG list_fragment_clusters = 250;

// The window of the tests that count windows: the smallest a profile may set,
// four records.
constexpr std::uint64_t small_window = 4096;

std::vector<BYTE> ListImage() {
  return TestFakeImage::BuildFakeNtfsImageWithListMftScan(
      list_record_count, list_fragment_clusters);
}

// Where the lists of the list fixture start: the $MFT lies below.
ULONGLONG ListsOffset() {
  return TestFakeImage::list_scan_lists_lcn * TestFakeImage::fake_cluster_size;
}

// The hints and reads of the scan that touch $MFT, not the lists.
std::vector<Call> MftCalls(const ScanResult& scan) {
  return Region(scan.calls, 0, ListsOffset());
}

// The hints and reads of the scan that touch the lists.
std::vector<Call> ListCalls(const ScanResult& scan) {
  return Region(scan.calls, ListsOffset(), ~ULONGLONG{0});
}

// A scan of image with the settings, next to the one without: only the reads
// MAY differ.
struct Pair {
  ScanResult base;
  ScanResult tuned;
};

Pair ScanBoth(const std::vector<BYTE>& image, const VolumeOptions& base,
              std::initializer_list<Setting> settings, unsigned threads = 1) {
  Pair pair;
  pair.base = ScanOf(image, base, threads);
  pair.tuned = ScanOf(image, Tuned(base, settings), threads);
  CHECK(pair.tuned.tree == pair.base.tree);
  return pair;
}

}  // namespace

TEST_CASE("mft.window_bytes sets the size of the reads of $MFT",
          "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image = ListImage();
  const std::uint64_t window = GENERATE(small_window, std::uint64_t{65536});
  INFO("window " << window);

  const Pair scans =
      ScanBoth(image, Recovering(), {{"mft.window_bytes", window}});

  // A window is read whole, or up to the end of a fragment of $MFT: the
  // default reads a fragment at a time.
  CHECK(LongestRead(scans.base.calls) > 65536);
  CHECK(LongestRead(scans.tuned.calls) <= window);
  CHECK(Count(scans.tuned.calls, Kind::Read) >
        Count(scans.base.calls, Kind::Read));
  CHECK(Count(scans.tuned.calls, Kind::Prefetch) >
        Count(scans.base.calls, Kind::Prefetch));
}

TEST_CASE("mft.max_announced_windows bounds the windows announced ahead",
          "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image = ListImage();
  const std::uint64_t ahead = GENERATE(std::uint64_t{1}, std::uint64_t{8});
  INFO("windows ahead " << ahead);

  const ScanResult wide =
      ScanOf(image, Tuned(Recovering(), {{"mft.window_bytes", small_window}}));
  const ScanResult narrow = ScanOf(
      image, Tuned(Recovering(), {{"mft.window_bytes", small_window},
                                  {"mft.max_announced_windows", ahead}}));

  const Outstanding wide_ahead = MaxOutstanding(MftCalls(wide));
  const Outstanding narrow_ahead = MaxOutstanding(MftCalls(narrow));
  CHECK(narrow.tree == wide.tree);
  // The window being read and the hints of the first reads add a few.
  CHECK(narrow_ahead.ranges <= ahead + 2);
  CHECK(narrow_ahead.ranges < wide_ahead.ranges);
  CHECK(Count(narrow.calls, Kind::Read) == Count(wide.calls, Kind::Read));
}

TEST_CASE("mft.max_announced_bytes bounds the bytes announced ahead",
          "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image = ListImage();
  const std::uint64_t bytes =
      GENERATE(std::uint64_t{4096}, std::uint64_t{16384});
  INFO("bytes ahead " << bytes);

  const ScanResult wide =
      ScanOf(image, Tuned(Recovering(), {{"mft.window_bytes", small_window}}));
  const ScanResult narrow =
      ScanOf(image, Tuned(Recovering(), {{"mft.window_bytes", small_window},
                                         {"mft.max_announced_bytes", bytes}}));

  const Outstanding wide_ahead = MaxOutstanding(MftCalls(wide));
  const Outstanding narrow_ahead = MaxOutstanding(MftCalls(narrow));
  CHECK(narrow.tree == wide.tree);
  CHECK(narrow_ahead.bytes <= bytes + (2 * small_window));
  CHECK(narrow_ahead.bytes < wide_ahead.bytes);
}

TEST_CASE("mft.max_gap_slots decides which free slots a window reads across",
          "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithMftBitmap(
          TestFakeImage::FakeMftBitmap::Resident);
  const VolumeOptions strict;

  const Pair scans = ScanBoth(image, strict, {{"mft.max_gap_slots", 0}});

  // The bitmap marks free slots 6 to 15, a gap of ten: the default reads
  // across it, and a gap of 0 reads the used slots on each side apart.
  CHECK(Count(scans.tuned.calls, Kind::Read) >
        Count(scans.base.calls, Kind::Read));
  std::uint64_t base_bytes = 0;
  std::uint64_t tuned_bytes = 0;
  for (const Call& call : scans.base.calls) {
    base_bytes += call.kind == Kind::Read ? call.length : 0;
  }
  for (const Call& call : scans.tuned.calls) {
    tuned_bytes += call.kind == Kind::Read ? call.length : 0;
  }
  CHECK(tuned_bytes < base_bytes);
}

TEST_CASE("mft.max_held_record_bytes sets what the second pass reads again",
          "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image = ListImage();

  const ScanResult all_held = ScanOf(image, Recovering());
  const ScanResult some_held = ScanOf(
      image, Tuned(Recovering(), {{"mft.max_held_record_bytes", 100000}}));
  const ScanResult none_held =
      ScanOf(image, Tuned(Recovering(), {{"mft.max_held_record_bytes", 0}}));

  CHECK(some_held.tree == all_held.tree);
  CHECK(none_held.tree == all_held.tree);
  CHECK(Count(some_held.calls, Kind::Read) > Count(all_held.calls, Kind::Read));
  CHECK(Count(none_held.calls, Kind::Read) >
        Count(some_held.calls, Kind::Read));
}

TEST_CASE("mft.max_preread_extensions sets the extension records read at once",
          "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image = ListImage();
  const VolumeOptions held_nothing =
      Tuned(Recovering(), {{"mft.max_held_record_bytes", 0}});

  const Pair scans =
      ScanBoth(image, held_nothing, {{"mft.max_preread_extensions", 0}});

  // The reads are the same: the records are read one by one either way, and
  // only the announcements of the batch go.
  CHECK(Count(scans.tuned.calls, Kind::Read) ==
        Count(scans.base.calls, Kind::Read));
  CHECK(Count(scans.tuned.calls, Kind::Prefetch) <
        Count(scans.base.calls, Kind::Prefetch));
}

TEST_CASE(
    "mft.max_run_slots sets how many records one read of the second pass "
    "covers",
    "[tuning][knob-effect][mft]") {
  // Ten files, each with six extension records in the slots after it.
  constexpr ULONGLONG extensions = 6;
  constexpr ULONGLONG groups = 10;
  const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithAdjacentExtensions(
          static_cast<ULONGLONG>(Mft::Idx::User) + (groups * (extensions + 1)),
          extensions);
  const VolumeOptions held_nothing =
      Tuned(VolumeOptions{}, {{"mft.max_held_record_bytes", 0}});

  const ScanResult whole = ScanOf(image, held_nothing);
  const ScanResult pairs =
      ScanOf(image, Tuned(held_nothing, {{"mft.max_run_slots", 2}}));
  const ScanResult singles =
      ScanOf(image, Tuned(held_nothing, {{"mft.max_run_slots", 1}}));

  CHECK(pairs.tree == whole.tree);
  CHECK(singles.tree == whole.tree);
  // A run of six reads once; of two, three times; of one, six times.
  CHECK(Count(singles.calls, Kind::Read) - Count(whole.calls, Kind::Read) ==
        groups * (extensions - 1));
  CHECK(Count(pairs.calls, Kind::Read) - Count(whole.calls, Kind::Read) ==
        groups * ((extensions / 2) - 1));
}

TEST_CASE(
    "mft.max_announced_records bounds the records the second pass announces",
    "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image = ListImage();
  const std::uint64_t ahead = GENERATE(std::uint64_t{1}, std::uint64_t{4});
  INFO("records ahead " << ahead);

  const Pair scans =
      ScanBoth(image, Recovering(), {{"mft.max_announced_records", ahead}});

  CHECK(MaxOutstanding(ListCalls(scans.tuned)).ranges == ahead);
  CHECK(MaxOutstanding(ListCalls(scans.base)).ranges > ahead);
  CHECK(Count(scans.tuned.calls, Kind::Read) ==
        Count(scans.base.calls, Kind::Read));
}

TEST_CASE(
    "mft.max_announced_ranges bounds the ranges the second pass announces",
    "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image = ListImage();
  const std::uint64_t ahead = GENERATE(std::uint64_t{1}, std::uint64_t{4});
  INFO("ranges ahead " << ahead);

  const Pair scans =
      ScanBoth(image, Recovering(), {{"mft.max_announced_ranges", ahead}});

  CHECK(MaxOutstanding(ListCalls(scans.tuned)).ranges == ahead);
  CHECK(MaxOutstanding(ListCalls(scans.base)).ranges > ahead);
}

TEST_CASE(
    "mft.announce_restart_interval sets how often the announcements restart",
    "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image = ListImage();

  const Pair scans =
      ScanBoth(image, Recovering(), {{"mft.announce_restart_interval", 40}});

  CHECK(Count(scans.tuned.calls, Kind::CancelPrefetch) >
        Count(scans.base.calls, Kind::CancelPrefetch));
  CHECK(Count(scans.tuned.calls, Kind::Read) ==
        Count(scans.base.calls, Kind::Read));
}

TEST_CASE(
    "mft.max_pending_ranges sets how many data-state queries one batch "
    "resolves",
    "[tuning][knob-effect][mft]") {
  const TestFakeImage::FakeManyDeleted built =
      TestFakeImage::BuildFakeNtfsImageWithManyDeletedFiles(8, 4);
  std::vector<BYTE> image = built.image;
  const std::vector<BYTE> bits = TestFakeImage::AllocatedBitmapFor(image, {});
  TestFakeImage::PutClusterBitmap(image, bits);

  const auto flushes = [&image](const VolumeOptions& options) {
    TestSupport::BenchSessionGuard session;
    const TestMft::ScannedVolume<Cache::Strategy::NoCache> scanned(image,
                                                                   options);
    const Bench::Report report = session.Finish();
    const auto flush = std::ranges::find(
        report.phases, Bench::Phase::DataStateFlush, &Bench::PhaseStat::phase);
    return flush == report.phases.end() ? 0U : flush->count;
  };

  const VolumeOptions deleted = TestAlloc::DeletedOptions();
  const std::uint32_t whole = flushes(deleted);
  const std::uint32_t one_by_one =
      flushes(Tuned(deleted, {{"mft.max_pending_ranges", 1}}));

  CHECK(whole == 1);
  CHECK(one_by_one > whole);
}

TEST_CASE("mft.max_kept_bitmap_bytes sets the bitmap windows a scan keeps",
          "[tuning][knob-effect][mft]") {
  const TestFakeImage::FakeManyDeleted built =
      TestFakeImage::BuildFakeNtfsImageWithManyDeletedFiles(8, 4);
  std::vector<BYTE> image = built.image;
  const std::vector<BYTE> bits = TestFakeImage::AllocatedBitmapFor(image, {});
  const ULONGLONG data_offset = image.size();
  TestFakeImage::PutClusterBitmap(image, bits);
  // Four windows of 32 clusters, so a batch of one file reloads some.
  Alloc::SetBitmapWindowBytesForTests(4);

  const auto bitmap_reads = [&](const VolumeOptions& options) {
    Mft::MftTree::ScanOptions scan;
    scan.cluster_account = Alloc::ClusterAccountLevel::Counts;
    const TestMft::ScannedVolume<Cache::Strategy::NoCache> scanned(
        image, options, scan);
    return scanned.opened.counter->ReadsInRange(data_offset, image.size());
  };

  const VolumeOptions one_by_one =
      Tuned(TestAlloc::DeletedOptions(), {{"mft.max_pending_ranges", 1}});
  const size_t kept = bitmap_reads(one_by_one);
  const size_t dropped =
      bitmap_reads(Tuned(one_by_one, {{"mft.max_kept_bitmap_bytes", 0}}));
  Alloc::SetBitmapWindowBytesForTests(0);

  CHECK(kept > 0);
  CHECK(dropped > kept);
}

TEST_CASE("The CPU and memory knobs of the scan change no read and no result",
          "[tuning][knob-effect][mft]") {
  const std::vector<BYTE> image = ListImage();
  const unsigned threads = GENERATE(1U, 3U);
  INFO("threads " << threads);
  const ScanResult base = ScanOf(image, Recovering(), threads);

  for (const Setting& setting :
       std::initializer_list<Setting>{{"mft.held_block_bytes", 4096},
                                      {"mft.parse_chunk_slots", 1},
                                      {"mft.waiting_chunk_records", 1},
                                      {"mft.waiting_batch_records", 3},
                                      {"mft.max_auto_threads", 1},
                                      {"mft.min_auto_threaded_slots", 0},
                                      {"mft.probe_slots", 1},
                                      {"mft.max_load_per_mille", 1000},
                                      {"mft.min_auto_threaded_waiting", 0}}) {
    INFO("knob " << setting.name);
    const ScanResult tuned =
        ScanOf(image, Tuned(Recovering(), {setting}), threads);
    CheckSameScan(base, tuned);
  }
}

TEST_CASE("An automatic scan of a small volume parses on one thread",
          "[tuning][knob-effect][mft]") {
  const ScanResult scan = ScanOf(ListImage(), Recovering(), 0);

  CHECK(scan.parsed_by_threads == 0);
  CHECK(scan.waiting_by_threads == 0);
}

TEST_CASE("The automatic thread knobs decide who parses a scan",
          "[tuning][knob-effect][mft]") {
  if (std::thread::hardware_concurrency() < 2) {
    SKIP("the scan takes the threads of the machine, and there is one");
  }
  const std::vector<BYTE> image = ListImage();
  // Windows of 64 slots: the probe decides between two of them.
  const VolumeOptions threaded =
      Tuned(Recovering(), {{"mft.window_bytes", 65536},
                           {"mft.min_auto_threaded_slots", 1000},
                           {"mft.probe_slots", 1000},
                           {"mft.max_load_per_mille", 1000}});

  const ScanResult plain = ScanOf(image, Recovering(), 0);
  const ScanResult taken = ScanOf(image, threaded, 0);
  const ScanResult later =
      ScanOf(image, Tuned(threaded, {{"mft.probe_slots", 2000}}), 0);
  const ScanResult never =
      ScanOf(image, Tuned(threaded, {{"mft.max_load_per_mille", 0}}), 0);
  const ScanResult too_big = ScanOf(
      image, Tuned(threaded, {{"mft.min_auto_threaded_slots", 100000}}), 0);

  // A volume of 3000 slots is below the default size; with the size lowered,
  // the threads take what follows the probe.
  CHECK(plain.parsed_by_threads == 0);
  CHECK(taken.parsed_by_threads > 0);
  CHECK(later.parsed_by_threads > 0);
  CHECK(later.parsed_by_threads < taken.parsed_by_threads);
  // The second pass takes threads from a count of waiting records on: the
  // fixture has fewer than the default count.
  CHECK(taken.waiting_by_threads == 0);
  const ScanResult waiting = ScanOf(
      image, Tuned(threaded, {{"mft.min_auto_threaded_waiting", 10}}), 0);
  CHECK(waiting.waiting_by_threads > 0);
  CHECK(waiting.tree == plain.tree);
  // The probe saw loads that took more than the share allowed.
  CHECK(never.parsed_by_threads == 0);
  CHECK(too_big.parsed_by_threads == 0);
  for (const ScanResult* scan : {&taken, &later, &never, &too_big}) {
    CHECK(scan->tree == plain.tree);
  }
}

TEST_CASE("mft.max_auto_threads caps the threads of an automatic scan",
          "[tuning][knob-effect][mft]") {
  if (std::thread::hardware_concurrency() < 3) {
    SKIP("the scan takes the threads of the machine, and there are too few");
  }
  const std::vector<BYTE> image = ListImage();
  const VolumeOptions threaded =
      Tuned(Recovering(), {{"mft.window_bytes", 65536},
                           {"mft.min_auto_threaded_slots", 1000},
                           {"mft.probe_slots", 1000},
                           {"mft.max_load_per_mille", 1000}});

  const auto pool_workers = [&](std::uint64_t cap) {
    TestSupport::BenchSessionGuard session;
    const ScanResult scan =
        ScanOf(image, Tuned(threaded, {{"mft.max_auto_threads", cap}}), 0);
    static_cast<void>(scan);
    const Bench::Report report = session.Finish();
    return std::ranges::count(report.threads, Bench::ThreadRole::PoolWorker,
                              &Bench::ThreadStat::role);
  };

  CHECK(pool_workers(1) == 0);
  const auto two = pool_workers(2);
  const auto three = pool_workers(3);
  CHECK(two > 0);
  CHECK(three > two);
}

TEST_CASE("ScanThreadsPay weighs the loads against the share the tuning allows",
          "[tuning][knob-effect][mft]") {
  CHECK(Mft::ScanThreadsPay(1.0, 10.0, 0.2));
  CHECK(Mft::ScanThreadsPay(2.0, 10.0, 0.2));
  CHECK_FALSE(Mft::ScanThreadsPay(3.0, 10.0, 0.2));
  CHECK(Mft::ScanThreadsPay(3.0, 10.0, 0.5));
  CHECK_FALSE(Mft::ScanThreadsPay(0.001, 10.0, 0.0));
  CHECK(Mft::ScanThreadsPay(10.0, 10.0, 1.0));
  CHECK_FALSE(Mft::ScanThreadsPay(0.0, 0.0, 1.0));
  // The share of the default is the 200 per mille of MftTuning.
  CHECK(Mft::ScanThreadsPay(2.0, 10.0) == Mft::ScanThreadsPay(2.0, 10.0, 0.2));
  CHECK(Mft::ScanThreadsPay(2.5, 10.0) == Mft::ScanThreadsPay(2.5, 10.0, 0.2));
}
