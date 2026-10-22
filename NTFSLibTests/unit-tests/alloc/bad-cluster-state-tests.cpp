#include <ntfs-browser/win-types.h>

#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-failure-support.h"
#include "alloc-test-support.h"
#include "fake-ntfs-image.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::ClusterState;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeBadClusterOptions;
using Tests::FakeDataRun;

namespace {

// Clusters the fixture marks allocated, besides the bitmap's own.
const std::vector<FakeDataRun> allocated_runs{
    FakeDataRun{.lcn = 40, .clusters = 2},
    FakeDataRun{.lcn = 50, .clusters = 1}};

// Bad clusters: 45 and 46 are marked allocated in the bitmap, as NTFS does.
const std::vector<Alloc::Extent> bad_marked{{.lcn = 45, .clusters = 2}};

// A bad cluster whose bit in the bitmap is clear: the two files disagree.
const std::vector<Alloc::Extent> bad_clear{{.lcn = 60, .clusters = 1}};

// The base image with a bitmap and a bad list. bad_in_bitmap is the part of
// the list the bitmap marks allocated; bad is the whole list.
std::vector<BYTE> ImageWith(std::span<const Alloc::Extent> bad,
                            std::span<const Alloc::Extent> bad_in_bitmap,
                            const FakeBadClusterOptions& options = {}) {
  std::vector<BYTE> image =
      Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(),
                          Support::WithBadRuns(allocated_runs, bad_in_bitmap));
  return Support::WithBadClusters(std::move(image), bad, options);
}

// Both lists together, in cluster order.
std::vector<Alloc::Extent> BothBadLists() {
  std::vector<Alloc::Extent> both = bad_marked;
  both.insert(both.end(), bad_clear.begin(), bad_clear.end());
  return both;
}

template <Cache::Strategy S>
void CheckBadClustersAreBad() {
  const std::vector<Alloc::Extent> both = BothBadLists();
  const auto opened = Support::Open<S>(ImageWith(both, bad_marked));
  const auto& volume = *opened.volume;

  CHECK(volume.GetClusterState(44) == ClusterState::Free);
  CHECK(volume.GetClusterState(45) == ClusterState::Bad);
  CHECK(volume.GetClusterState(46) == ClusterState::Bad);
  CHECK(volume.GetClusterState(47) == ClusterState::Free);
  CHECK(volume.GetClusterState(60) == ClusterState::Bad);
  CHECK(volume.GetClusterState(40) == ClusterState::Allocated);
  CHECK(volume.GetClusterState(50) == ClusterState::Allocated);
  CHECK(volume.GetRangeState(44, 4) == ClusterState::Bad);
  CHECK(volume.GetRangeState(38, 12) == ClusterState::Allocated);
}

}  // namespace

TEST_CASE("a volume tells a bad cluster from a free and an allocated one",
          "[alloc][bad-state]") {
  SECTION("NoCache") { CheckBadClustersAreBad<Cache::Strategy::NoCache>(); }
  SECTION("FullCache") { CheckBadClustersAreBad<Cache::Strategy::FullCache>(); }
}

namespace {

// The message of a bad cluster that the bitmap calls free.
constexpr std::string_view disagreement_message =
    "Clusters listed in $BadClus are free in $Bitmap";

template <Cache::Strategy S>
void CheckDisagreementIsLoggedOnce() {
  const auto sink = std::make_shared<Support::LevelSink>();
  const Support::ScopedSink scoped(sink);
  const auto opened = Support::Open<S>(ImageWith(bad_clear, {}));

  CHECK(opened.volume->GetClusterState(60) == ClusterState::Bad);
  CHECK(opened.volume->GetClusterState(60) == ClusterState::Bad);
  CHECK(opened.volume->GetRangeState(58, 5) == ClusterState::Bad);

  CHECK(sink->Count(disagreement_message) == 1);
}

template <Cache::Strategy S>
void CheckRangesCountTheirBadClusters() {
  const std::vector<Alloc::Extent> both = BothBadLists();
  const auto opened = Support::Open<S>(ImageWith(both, bad_marked));

  std::vector<Alloc::RangeQuery> queries{
      {.lcn = 44, .clusters = 4},   // 45, 46 are bad and marked
      {.lcn = 38, .clusters = 12},  // 40, 41 allocated, 45, 46 bad
      {.lcn = 58, .clusters = 5},   // 60 is bad and free in the bitmap
      {.lcn = 20, .clusters = 5}};  // nothing
  opened.volume->GetRangeStates(queries);

  CHECK(queries[0].state == ClusterState::Bad);
  CHECK(queries[0].bad_clusters == 2);
  CHECK(queries[0].allocated_clusters == 0);
  CHECK(queries[0].free_clusters == 2);

  CHECK(queries[1].state == ClusterState::Allocated);
  CHECK(queries[1].bad_clusters == 2);
  CHECK(queries[1].allocated_clusters == 2);

  CHECK(queries[2].state == ClusterState::Bad);
  CHECK(queries[2].bad_clusters == 1);
  CHECK(queries[2].free_clusters == 4);

  CHECK(queries[3].state == ClusterState::Free);
  CHECK(queries[3].bad_clusters == 0);
}

template <Cache::Strategy S>
void CheckAHealthyVolumeHasAnEmptyList() {
  const auto opened = Support::Open<S>(ImageWith({}, {}));

  const Alloc::BadClusters* list = opened.volume->GetBadClusters();
  REQUIRE(list != nullptr);
  CHECK(list->extents.empty());
  CHECK_FALSE(list->truncated);
  CHECK(opened.volume->GetClusterState(45) == ClusterState::Free);
}

template <Cache::Strategy S>
void CheckTheListIsSortedMergedAndClamped() {
  // Out of order, touching (10..12 and 13..14), and one past the volume.
  const std::vector<Alloc::Extent> given{{.lcn = 50, .clusters = 2},
                                         {.lcn = 10, .clusters = 3},
                                         {.lcn = 13, .clusters = 2},
                                         {.lcn = 1000, .clusters = 4}};
  const auto opened = Support::Open<S>(ImageWith(given, {}));

  const Alloc::BadClusters* list = opened.volume->GetBadClusters();
  REQUIRE(list != nullptr);
  REQUIRE(list->extents.size() == 2);
  CHECK(list->extents[0].lcn == 10);
  CHECK(list->extents[0].clusters == 5);
  CHECK(list->extents[1].lcn == 50);
  CHECK(list->extents[1].clusters == 2);
  CHECK_FALSE(list->truncated);
  // The same pointer comes back: the list is read once.
  CHECK(opened.volume->GetBadClusters() == list);
}

template <Cache::Strategy S>
void CheckOptionsDoNotChangeTheAnswer() {
  const std::vector<Alloc::Extent> both = BothBadLists();
  const std::vector<BYTE> image = ImageWith(both, bad_marked);

  for (const bool include_deleted : {false, true}) {
    const auto opened = Support::Open<S>(
        image, VolumeOptions{.include_deleted = include_deleted});

    CHECK(opened.volume->GetClusterState(45) == ClusterState::Bad);
    CHECK(opened.volume->GetClusterState(60) == ClusterState::Bad);
    CHECK(opened.volume->GetClusterState(47) == ClusterState::Free);
    REQUIRE(opened.volume->GetBadClusters() != nullptr);
    CHECK(opened.volume->GetBadClusters()->extents.size() == 2);
  }
}

template <Cache::Strategy S>
void CheckPastTheEndIsOutOfRange() {
  const auto opened = Support::Open<S>(ImageWith(bad_marked, bad_marked));

  CHECK(opened.volume->GetClusterState(1000000) == ClusterState::OutOfRange);
  CHECK(opened.volume->GetClusterState(std::numeric_limits<ULONGLONG>::max()) ==
        ClusterState::OutOfRange);
}

}  // namespace

TEST_CASE("a bad cluster that the bitmap calls free is logged once",
          "[alloc][bad-state]") {
  CheckDisagreementIsLoggedOnce<Cache::Strategy::NoCache>();
  CheckDisagreementIsLoggedOnce<Cache::Strategy::FullCache>();
}

TEST_CASE("a range counts its bad clusters apart", "[alloc][bad-state]") {
  CheckRangesCountTheirBadClusters<Cache::Strategy::NoCache>();
  CheckRangesCountTheirBadClusters<Cache::Strategy::FullCache>();
}

TEST_CASE("a healthy volume has a present, empty bad list",
          "[alloc][bad-state]") {
  CheckAHealthyVolumeHasAnEmptyList<Cache::Strategy::NoCache>();
  CheckAHealthyVolumeHasAnEmptyList<Cache::Strategy::FullCache>();
}

TEST_CASE("the bad list comes out sorted, merged and clamped",
          "[alloc][bad-state]") {
  CheckTheListIsSortedMergedAndClamped<Cache::Strategy::NoCache>();
  CheckTheListIsSortedMergedAndClamped<Cache::Strategy::FullCache>();
}

TEST_CASE("the bad list does not depend on include_deleted",
          "[alloc][bad-state]") {
  CheckOptionsDoNotChangeTheAnswer<Cache::Strategy::NoCache>();
  CheckOptionsDoNotChangeTheAnswer<Cache::Strategy::FullCache>();
}

TEST_CASE("a cluster past the end of the volume is still out of range",
          "[alloc][bad-state]") {
  CheckPastTheEndIsOutOfRange<Cache::Strategy::NoCache>();
  CheckPastTheEndIsOutOfRange<Cache::Strategy::FullCache>();
}
