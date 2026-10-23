#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-failure-support.h"
#include "alloc-test-support.h"
#include "alloc/bad-cluster-list.h"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::ClusterState;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeDataRun;

namespace {

// The ceiling these tests use: small enough to cut with a few extents.
constexpr size_t small_ceiling = 8;

// What the library says when it cuts or rejects a list over the ceiling.
constexpr std::string_view over_message = "more than 8 extents";

// Restores the default ceiling when a test case ends.
struct CeilingGuard {
  explicit CeilingGuard(size_t ceiling) {
    Alloc::SetBadExtentCeilingForTests(ceiling);
  }

  CeilingGuard(const CeilingGuard&) = delete;
  CeilingGuard& operator=(const CeilingGuard&) = delete;

  ~CeilingGuard() { Alloc::SetBadExtentCeilingForTests(0); }
};

// count isolated one-cluster extents from cluster 10, two clusters apart.
std::vector<Alloc::Extent> Isolated(size_t count) {
  std::vector<Alloc::Extent> extents;
  for (size_t i = 0; i < count; i++) {
    extents.push_back({.lcn = 10 + 2 * ULONGLONG{i}, .clusters = 1});
  }
  return extents;
}

// The freed tree with a bitmap that marks the last extent's cluster allocated,
// so a cluster past the cut is told apart from a free one.
std::vector<BYTE> Image(const std::vector<Alloc::Extent>& bad) {
  const std::vector<FakeDataRun> marked{
      FakeDataRun{.lcn = bad.back().lcn, .clusters = 1}};
  std::vector<BYTE> image =
      Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(), marked);
  return Support::WithBadClusters(std::move(image), bad);
}

// A volume over a bad list of count isolated extents, opened under the small
// ceiling with the library's messages captured.
template <Cache::Strategy S>
struct CeilingVolume {
  explicit CeilingVolume(
      size_t count, const VolumeOptions& options = Support::DeletedOptions())
      : opened(Support::Open<S>(Image(Isolated(count)), options)) {}

  const CeilingGuard guard{small_ceiling};
  const Support::CapturedLevels log;
  Support::Opened<S> opened;
};

template <Cache::Strategy S>
void CheckStrictRejectsAListOverTheCeiling() {
  const CeilingVolume<S> fixture(small_ceiling + 1);
  const NtfsVolume<S>& volume = *fixture.opened.volume;

  CHECK(volume.GetBadClusters() == nullptr);
  CHECK(fixture.log.Count(over_message) == 1);
  CHECK(fixture.log.Count("$BadClus is not usable") == 1);
  // The bitmap alone answers: the last cluster is allocated, the others free.
  CHECK(volume.GetClusterState(26) == ClusterState::Allocated);
  CHECK(volume.GetClusterState(10) == ClusterState::Free);
}

template <Cache::Strategy S>
void CheckRecoverCutsAListOverTheCeiling() {
  const CeilingVolume<S> fixture(
      small_ceiling + 1,
      VolumeOptions{.include_deleted = true, .recover_errors = true});
  const NtfsVolume<S>& volume = *fixture.opened.volume;

  const Alloc::BadClusters& list = Support::RequiredBadList(volume);
  CHECK(list.truncated);
  REQUIRE(list.extents.size() == small_ceiling);
  CHECK(list.extents.front().lcn == 10);
  CHECK(list.extents.back().lcn == 24);
  CHECK(fixture.log.Count(over_message) == 1);
  CHECK(fixture.log.LevelOf(over_message) == Log::Level::Info);

  CHECK(volume.GetClusterState(24) == ClusterState::Bad);
  // Past the last kept extent the bitmap alone answers.
  CHECK(volume.GetClusterState(26) == ClusterState::Allocated);
  CHECK(volume.GetClusterState(25) == ClusterState::Free);
}

template <Cache::Strategy S>
void CheckAListAtTheCeilingIsWhole() {
  for (const bool recover : {false, true}) {
    const CeilingVolume<S> fixture(
        small_ceiling,
        VolumeOptions{.include_deleted = true, .recover_errors = recover});

    const Alloc::BadClusters& list =
        Support::RequiredBadList(*fixture.opened.volume);
    CHECK_FALSE(list.truncated);
    CHECK(list.extents.size() == small_ceiling);
  }
}

}  // namespace

TEST_CASE("a list over the ceiling is rejected when strict",
          "[alloc][bad-ceiling]") {
  CheckStrictRejectsAListOverTheCeiling<Cache::Strategy::NoCache>();
  CheckStrictRejectsAListOverTheCeiling<Cache::Strategy::FullCache>();
}

TEST_CASE("a list over the ceiling is cut and flagged when recovering",
          "[alloc][bad-ceiling]") {
  CheckRecoverCutsAListOverTheCeiling<Cache::Strategy::NoCache>();
  CheckRecoverCutsAListOverTheCeiling<Cache::Strategy::FullCache>();
}

TEST_CASE("a list at the ceiling is whole in both modes",
          "[alloc][bad-ceiling]") {
  CheckAListAtTheCeilingIsWhole<Cache::Strategy::NoCache>();
  CheckAListAtTheCeilingIsWhole<Cache::Strategy::FullCache>();
}
