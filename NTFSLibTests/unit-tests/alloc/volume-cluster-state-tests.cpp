#include <ntfs-browser/win-types.h>

#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::ClusterState;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeBitmapOptions;
using Tests::FakeBitmapShape;
using Tests::FakeDataRun;

namespace {

// Clusters the fixture marks allocated, besides the bitmap's own.
const std::vector<FakeDataRun> allocated_runs{
    FakeDataRun{.lcn = 40, .clusters = 2},
    FakeDataRun{.lcn = 50, .clusters = 1}};

std::vector<BYTE> ImageFor(FakeBitmapShape shape) {
  return Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(),
                             allocated_runs, FakeBitmapOptions{.shape = shape});
}

// Opens the volume whose bitmap has the given shape.
template <Cache::Strategy S>
Support::Opened<S>
    OpenWithShape(FakeBitmapShape shape = FakeBitmapShape::NonResident) {
  return Support::Open<S>(ImageFor(shape));
}

template <Cache::Strategy S>
void CheckStatesMatchBits(FakeBitmapShape shape) {
  const auto opened = OpenWithShape<S>(shape);
  const auto& volume = *opened.volume;

  CHECK(volume.GetClusterState(39) == ClusterState::Free);
  CHECK(volume.GetClusterState(40) == ClusterState::Allocated);
  CHECK(volume.GetClusterState(41) == ClusterState::Allocated);
  CHECK(volume.GetClusterState(42) == ClusterState::Free);
  CHECK(volume.GetClusterState(50) == ClusterState::Allocated);
  CHECK(volume.GetRangeState(30, 10) == ClusterState::Free);
  CHECK(volume.GetRangeState(30, 11) == ClusterState::Allocated);
}

template <Cache::Strategy S>
void CheckFirstBitmapClusterIsAllocated() {
  const auto opened = OpenWithShape<S>();

  // The bitmap's own cluster follows the 64 clusters of the base image.
  CHECK(opened.volume->GetClusterState(Support::base_volume_clusters) ==
        ClusterState::Allocated);
}

template <Cache::Strategy S>
void CheckPastTheEndIsOutOfRange() {
  const auto opened = OpenWithShape<S>();
  const auto& volume = *opened.volume;

  CHECK(volume.GetClusterState(1000000) == ClusterState::OutOfRange);
  CHECK(volume.GetClusterState(std::numeric_limits<ULONGLONG>::max()) ==
        ClusterState::OutOfRange);
  CHECK(volume.GetRangeState(std::numeric_limits<ULONGLONG>::max() - 1, 10) ==
        ClusterState::OutOfRange);
}

template <Cache::Strategy S>
void CheckOptionsDoNotChangeTheAnswer() {
  const std::vector<BYTE> image = ImageFor(FakeBitmapShape::NonResident);

  for (const bool include_deleted : {false, true}) {
    for (const bool recover : {false, true}) {
      const auto opened = Support::Open<S>(
          image, VolumeOptions{.include_deleted = include_deleted,
                               .recover_errors = recover});
      CHECK(opened.volume->GetClusterState(40) == ClusterState::Allocated);
      CHECK(opened.volume->GetClusterState(39) == ClusterState::Free);
      CHECK(opened.volume->GetClusterState(1000000) ==
            ClusterState::OutOfRange);
    }
  }
}

template <Cache::Strategy S>
void CheckBatchFillsTheCounts() {
  const auto opened = OpenWithShape<S>();

  std::vector<Alloc::RangeQuery> queries{
      {.lcn = 38, .clusters = 6},
      {.lcn = 10, .clusters = 4},
      {.lcn = 48, .clusters = 4},
  };
  opened.volume->GetRangeStates(queries);

  CHECK(queries[0].state == ClusterState::Allocated);
  CHECK(queries[0].allocated_clusters == 2);
  CHECK(queries[0].free_clusters == 4);
  CHECK(queries[1].state == ClusterState::Free);
  CHECK(queries[1].allocated_clusters == 0);
  CHECK(queries[1].free_clusters == 4);
  CHECK(queries[2].state == ClusterState::Allocated);
  CHECK(queries[2].allocated_clusters == 1);
  CHECK(queries[2].free_clusters == 3);
  CHECK(queries[0].lcn == 38);
}

}  // namespace

TEST_CASE("a volume answers cluster states from a non-resident bitmap",
          "[alloc][volume]") {
  CheckStatesMatchBits<Cache::Strategy::NoCache>(FakeBitmapShape::NonResident);
  CheckStatesMatchBits<Cache::Strategy::FullCache>(
      FakeBitmapShape::NonResident);
}

TEST_CASE("a volume answers cluster states from a resident bitmap",
          "[alloc][volume]") {
  CheckStatesMatchBits<Cache::Strategy::NoCache>(FakeBitmapShape::Resident);
  CheckStatesMatchBits<Cache::Strategy::FullCache>(FakeBitmapShape::Resident);
}

TEST_CASE("a volume answers cluster states from a fragmented bitmap",
          "[alloc][volume]") {
  CheckStatesMatchBits<Cache::Strategy::NoCache>(FakeBitmapShape::Fragmented);
  CheckStatesMatchBits<Cache::Strategy::FullCache>(FakeBitmapShape::Fragmented);
}

TEST_CASE("the clusters of the bitmap itself are allocated",
          "[alloc][volume]") {
  CheckFirstBitmapClusterIsAllocated<Cache::Strategy::NoCache>();
  CheckFirstBitmapClusterIsAllocated<Cache::Strategy::FullCache>();
}

TEST_CASE("a cluster past the end of the volume is out of range",
          "[alloc][volume]") {
  CheckPastTheEndIsOutOfRange<Cache::Strategy::NoCache>();
  CheckPastTheEndIsOutOfRange<Cache::Strategy::FullCache>();
}

TEST_CASE("the volume options do not change a cluster state",
          "[alloc][volume]") {
  CheckOptionsDoNotChangeTheAnswer<Cache::Strategy::NoCache>();
  CheckOptionsDoNotChangeTheAnswer<Cache::Strategy::FullCache>();
}

TEST_CASE("a batch fills the allocated and free counts of each query",
          "[alloc][volume]") {
  CheckBatchFillsTheCounts<Cache::Strategy::NoCache>();
  CheckBatchFillsTheCounts<Cache::Strategy::FullCache>();
}
