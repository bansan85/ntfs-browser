#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "alloc-test-support.h"
#include "alloc/bad-cluster-list.h"
#include "alloc/cluster-bitmap.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Tests::FakeBitmapOptions;
using Tests::FakeBitmapShape;
using Tests::FakeDataRun;

namespace {

// Clusters the fixture marks allocated, besides the bitmap's own.
const std::vector<FakeDataRun> allocated_runs{
    FakeDataRun{.lcn = 40, .clusters = 2},
    FakeDataRun{.lcn = 50, .clusters = 1}};

// 45 and 46 are marked allocated in the bitmap, as NTFS does.
const std::vector<Alloc::Extent> bad_marked{{.lcn = 45, .clusters = 2}};

// 60 is clear in the bitmap: the two files disagree.
const std::vector<Alloc::Extent> bad_clear{{.lcn = 60, .clusters = 1}};

// The base image with a bitmap and, when bad is not empty, a bad list.
std::vector<BYTE> Image(std::span<const Alloc::Extent> bad,
                        std::span<const Alloc::Extent> marked) {
  std::vector<BYTE> image =
      Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(),
                          Support::WithBadRuns(allocated_runs, marked));
  if (!bad.empty()) {
    image = Support::WithBadClusters(std::move(image), bad);
  }
  return image;
}

template <Cache::Strategy S>
std::optional<Alloc::SpaceSummary> SummaryOf(std::vector<BYTE> image) {
  const auto opened = Support::Open<S>(std::move(image));
  return opened.volume->GetSpaceSummary();
}

template <Cache::Strategy S>
void CheckBadClustersAreReportedApart() {
  std::vector<Alloc::Extent> both = bad_marked;
  both.insert(both.end(), bad_clear.begin(), bad_clear.end());

  const auto plain = SummaryOf<S>(Image({}, {}));
  const auto with_list = SummaryOf<S>(Image(both, bad_marked));
  REQUIRE(plain);
  REQUIRE(with_list);

  CHECK(with_list->total_clusters == plain->total_clusters);
  CHECK(with_list->bad_clusters == 3);
  CHECK(with_list->bad_bytes == 3 * Tests::fake_cluster_size);
  // 45 and 46 were allocated in the plain image's own bitmap as well, and are
  // not free in either. 60 was free there and is bad here.
  CHECK(with_list->unknown_clusters == plain->unknown_clusters);
  CHECK(plain->bad_clusters == 0);
  CHECK(plain->bad_bytes == 0);
}

template <Cache::Strategy S>
void CheckABadClusterIsNeverFree() {
  // The same listed cluster, with its bit set and with it clear: it is bad in
  // both, and never free.
  const auto marked = SummaryOf<S>(Image(bad_marked, bad_marked));
  const auto clear = SummaryOf<S>(Image(bad_marked, {}));
  const auto none = SummaryOf<S>(Image({}, {}));
  REQUIRE(marked);
  REQUIRE(clear);
  REQUIRE(none);

  CHECK(marked->bad_clusters == 2);
  CHECK(clear->bad_clusters == 2);
  // Marked: the bits were set, so the two clusters were never free. Clear: they
  // were free in the bitmap and are no longer, so the free count falls just as
  // far.
  CHECK(marked->free_clusters == none->free_clusters - 2);
  CHECK(clear->free_clusters == none->free_clusters - 2);
}

template <Cache::Strategy S>
void CheckAShortBitmapDoesNotHideABadCluster() {
  // A bitmap of 4 bytes covers clusters 0 to 31 of a volume of 64 or more.
  const std::vector<Alloc::Extent> past{{.lcn = 45, .clusters = 2}};
  const auto build = [&](std::span<const Alloc::Extent> bad) {
    std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
    const std::vector<BYTE> bits(4, 0);
    Tests::PutClusterBitmap(image, bits, {.pad_to_volume = false});
    if (!bad.empty()) {
      image = Support::WithBadClusters(std::move(image), bad);
    }
    return image;
  };

  const auto plain = SummaryOf<S>(build({}));
  const auto with_list = SummaryOf<S>(build(past));
  REQUIRE(plain);
  REQUIRE(with_list);

  CHECK(plain->unknown_clusters > 0);
  CHECK(with_list->bad_clusters == 2);
  CHECK(with_list->unknown_clusters == plain->unknown_clusters - 2);
}

template <Cache::Strategy S>
void CheckAnUnusableBitmapGivesNoReport() {
  // Record 6 is zero-filled in the freed tree: there is no bitmap.
  const std::vector<BYTE> image = Support::WithBadClusters(
      Tests::BuildFakeNtfsImageWithFreedFileTree(), bad_marked);
  const auto opened = Support::Open<S>(image);

  CHECK_FALSE(opened.volume->GetSpaceSummary());
  CHECK(opened.volume->GetClusterState(45) == Alloc::ClusterState::Bad);
  CHECK(opened.volume->GetClusterState(46) == Alloc::ClusterState::Bad);
  CHECK(opened.volume->GetClusterState(47) == Alloc::ClusterState::Unknown);
  CHECK(opened.volume->GetClusterState(1) == Alloc::ClusterState::Unknown);
}

}  // namespace

TEST_CASE("the space report counts the bad clusters apart",
          "[alloc][bad-summary]") {
  CheckBadClustersAreReportedApart<Cache::Strategy::NoCache>();
  CheckBadClustersAreReportedApart<Cache::Strategy::FullCache>();
}

TEST_CASE("a bad cluster is never counted free, whatever its bit",
          "[alloc][bad-summary]") {
  CheckABadClusterIsNeverFree<Cache::Strategy::NoCache>();
  CheckABadClusterIsNeverFree<Cache::Strategy::FullCache>();
}

TEST_CASE("a short bitmap does not hide a bad cluster",
          "[alloc][bad-summary]") {
  CheckAShortBitmapDoesNotHideABadCluster<Cache::Strategy::NoCache>();
  CheckAShortBitmapDoesNotHideABadCluster<Cache::Strategy::FullCache>();
}

TEST_CASE("an unusable bitmap gives no report, but keeps the bad clusters",
          "[alloc][bad-summary]") {
  CheckAnUnusableBitmapGivesNoReport<Cache::Strategy::NoCache>();
  CheckAnUnusableBitmapGivesNoReport<Cache::Strategy::FullCache>();
}

TEST_CASE("the bad size in bytes saturates instead of wrapping",
          "[alloc][bad-summary]") {
  // 2^40 bad clusters of 2^32 - 1 bytes would not fit 64 bits.
  constexpr ULONGLONG clusters = ULONGLONG{1} << 40;
  const auto reader = [](ULONGLONG,
                         std::span<BYTE> out) -> std::optional<ULONGLONG> {
    std::fill(out.begin(), out.end(), BYTE{0});
    return out.size();
  };
  Alloc::ClusterBitmap bitmap(reader, 8, clusters);
  const auto list = Alloc::BadClusterList::Build(
      {.extents = {{.lcn = 0, .clusters = clusters}}, .complete = true},
      clusters, false);
  REQUIRE(list != nullptr);
  bitmap.SetBadList(list.get());

  const auto summary = bitmap.Summary(std::numeric_limits<DWORD>::max());

  REQUIRE(summary);
  CHECK(summary->bad_clusters == clusters);
  CHECK(summary->bad_bytes == std::numeric_limits<ULONGLONG>::max());
}
