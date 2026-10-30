#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "directory-read-ahead-support.h"
#include "fake-ntfs-image.h"
#include "throttled-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace ReadAhead = NTFS_BROWSER_NAMESPACE::Tests::ReadAhead;

namespace {

// The tree the gain is measured on: 80 small directories, enough for a window
// of eight to slide many times.
constexpr std::size_t groups = 8;
constexpr std::size_t directories_per_group = 10;

// A budget that never binds.
constexpr std::uint64_t large_budget = std::uint64_t{16} << 20U;

// The window the gain is measured with.
constexpr std::size_t window = 8;

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "The read ahead announces the blocks of a recursive listing",
    "[io][directory-read-ahead]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const auto image =
      Tests::BuildFakeNtfsImageWithNameTree(ReadAhead::BigDirectoryTree(6));

  const ReadAhead::TimedRun off =
      ReadAhead::ListOnSimulatedDisk<S>(image, {}, 0, 0, Tests::DiskModel{});
  const ReadAhead::TimedRun on = ReadAhead::ListOnSimulatedDisk<S>(
      image, {}, window, large_budget, Tests::DiskModel{});

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
      ReadAhead::ListOnSimulatedDisk<S>(image, {}, 0, 0, hard_disk);
  const ReadAhead::TimedRun on = ReadAhead::ListOnSimulatedDisk<S>(
      image, {}, window, large_budget, hard_disk);

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
      ReadAhead::ListOnSimulatedDisk<S>(image, {}, 0, 0, solid_state);
  const ReadAhead::TimedRun on = ReadAhead::ListOnSimulatedDisk<S>(
      image, {}, window, large_budget, solid_state);

  CAPTURE(off.elapsed_ns, on.elapsed_ns, off.disk.disk_reads,
          on.disk.disk_reads);
  CHECK(on.run.text == off.run.text);
  CHECK(on.elapsed_ns <= off.elapsed_ns);
}
