#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "directory-read-ahead-support.h"
#include "fake-ntfs-image.h"
#include "throttled-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace ReadAhead = NTFS_BROWSER_NAMESPACE::Tests::ReadAhead;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// The tree the gain is measured on: 80 small directories, enough for a window
// of eight to slide many times.
constexpr std::size_t groups = 8;
constexpr std::size_t directories_per_group = 10;

// A budget that never binds.
constexpr std::uint64_t large_budget = std::uint64_t{16} << 20U;

// The window the gain is measured with.
constexpr std::size_t window = 8;

// The tree the lane is measured on: sibling directories more than 64 records
// apart, the most that a run of $MFT reads across, so that the records the
// probes read are not those a batch has read already.
constexpr std::size_t spaced_directories = 8;
constexpr std::size_t spaced_files = 6;
constexpr std::size_t spaced_ballast = 70;

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "The read ahead announces the blocks of a recursive listing",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image =
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::BigDirectoryTree(6));

  const ReadAhead::TimedRun off =
      ReadAhead::ListOnSimulatedDisk<S>(image, {}, 0, 0, {Tests::DiskModel{}});
  const ReadAhead::TimedRun on = ReadAhead::ListOnSimulatedDisk<S>(
      image, {}, window, large_budget, {Tests::DiskModel{}});

  CHECK(on.run.text == off.run.text);
  CHECK(on.disk.unannounced_reads < off.disk.unannounced_reads);
  CHECK(on.disk.wasted_ranges <= off.disk.wasted_ranges);
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead shortens a recursive listing of many small directories on "
    "a hard disk",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::GroupedTree(groups, directories_per_group));
  const Tests::DiskModel hard_disk = ReadAhead::HardDiskModel();

  const ReadAhead::TimedRun off =
      ReadAhead::ListOnSimulatedDisk<S>(image, {}, 0, 0, {hard_disk});
  const ReadAhead::TimedRun on = ReadAhead::ListOnSimulatedDisk<S>(
      image, {}, window, large_budget, {hard_disk});

  CAPTURE(off.elapsed_ns, on.elapsed_ns, off.disk.disk_reads,
          on.disk.disk_reads);
  CHECK(on.run.text == off.run.text);
  CHECK(on.elapsed_ns <= off.elapsed_ns);
  if constexpr (S == Cache::Strategy::NoCache) {
    CHECK(on.elapsed_ns < off.elapsed_ns);
  }
}

TEMPLATE_TEST_CASE_SIG(
    "The read ahead does not slow a recursive listing on a solid state disk",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::GroupedTree(groups, directories_per_group));
  const Tests::DiskModel solid_state;

  const ReadAhead::TimedRun off =
      ReadAhead::ListOnSimulatedDisk<S>(image, {}, 0, 0, {solid_state});
  const ReadAhead::TimedRun on = ReadAhead::ListOnSimulatedDisk<S>(
      image, {}, window, large_budget, {solid_state});

  CAPTURE(off.elapsed_ns, on.elapsed_ns, off.disk.disk_reads,
          on.disk.disk_reads);
  CHECK(on.run.text == off.run.text);
  CHECK(on.elapsed_ns <= off.elapsed_ns);
}

TEMPLATE_TEST_CASE_SIG(
    "The standing lane shortens a recursive listing on a solid state disk",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::SpacedTree(spaced_directories, spaced_files, spaced_ballast));
  const Tests::DiskModel solid_state;
  VolumeOptions without_lane;
  without_lane.tuning.walk.read_ahead_runs = 0;

  const ReadAhead::TimedRun off = ReadAhead::ListOnSimulatedDisk<S>(
      image, without_lane, window, large_budget, {solid_state});
  const ReadAhead::TimedRun on = ReadAhead::ListOnSimulatedDisk<S>(
      image, {}, window, large_budget, {solid_state});

  CAPTURE(off.elapsed_ns, on.elapsed_ns, off.disk.disk_reads,
          on.disk.disk_reads, off.disk.unannounced_reads,
          on.disk.unannounced_reads, on.disk.wasted_ranges);
  CHECK(on.run.text == off.run.text);
  CHECK(on.elapsed_ns < off.elapsed_ns);
}

TEMPLATE_TEST_CASE_SIG(
    "The standing lane does not change a recursive listing on a rotational "
    "disk",
    "[io][directory-read-ahead][standing-lane]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image = Tests::BuildFakeNtfsImageWithNameTree(
      ReadAhead::GroupedTree(groups, directories_per_group));
  const Tests::DiskModel hard_disk = ReadAhead::HardDiskModel();
  VolumeOptions without_lane;
  without_lane.tuning.walk.read_ahead_runs = 0;

  const ReadAhead::TimedRun off = ReadAhead::ListOnSimulatedDisk<S>(
      image, without_lane, window, large_budget,
      {hard_disk, Tuning::MediaKind::Rotational});
  const ReadAhead::TimedRun on = ReadAhead::ListOnSimulatedDisk<S>(
      image, {}, window, large_budget,
      {hard_disk, Tuning::MediaKind::Rotational});

  CAPTURE(off.elapsed_ns, on.elapsed_ns);
  CHECK(on.run.text == off.run.text);
  CHECK(on.elapsed_ns == off.elapsed_ns);
  CHECK(on.disk.disk_reads == off.disk.disk_reads);
  CHECK(on.disk.disk_bytes == off.disk.disk_bytes);
}
