#include <ntfs-browser/win-types.h>

#include <array>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "fake-ntfs-image.h"

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

// Clusters the fixture marks allocated: three, plus the bitmap's own cluster.
constexpr ULONGLONG allocated_in_fixture = 4;

// The freed tree with a bitmap that marks the clusters 40, 41 and 50.
std::vector<BYTE> TreeWithBitmap() {
  const std::vector<FakeDataRun> allocated{
      FakeDataRun{.lcn = 40, .clusters = 2},
      FakeDataRun{.lcn = 50, .clusters = 1}};
  return Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(),
                             allocated);
}

// The freed tree with a resident bitmap of exactly these bytes.
std::vector<BYTE> TreeWithResidentBitmap(const std::vector<BYTE>& bytes) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutClusterBitmap(image, bytes,
                          FakeBitmapOptions{.shape = FakeBitmapShape::Resident,
                                            .pad_to_volume = false});
  return image;
}

template <Cache::Strategy S>
void CheckFreeSpaceIsTheKnownValue() {
  const std::vector<BYTE> image = TreeWithBitmap();
  const ULONGLONG total = image.size() / Tests::fake_cluster_size;
  const auto opened = Support::Open<S>(image);

  const std::optional<Alloc::SpaceSummary> summary =
      opened.volume->GetSpaceSummary();
  REQUIRE(summary);
  CHECK(summary->total_clusters == total);
  CHECK(summary->free_clusters == total - allocated_in_fixture);
  CHECK(summary->free_bytes ==
        (total - allocated_in_fixture) * Tests::fake_cluster_size);
  CHECK(summary->unknown_clusters == 0);
}

template <Cache::Strategy S>
void CheckPaddingBitsDoNotCount() {
  // 64 clusters: eight bytes of bits, then a ninth byte that is all padding.
  std::vector<BYTE> bytes(9, 0x00);
  bytes.at(0) = 0x08;
  bytes.at(8) = 0xFF;
  const auto opened = Support::Open<S>(TreeWithResidentBitmap(bytes));

  const auto summary = opened.volume->GetSpaceSummary();
  REQUIRE(summary);
  CHECK(summary->total_clusters == 64);
  CHECK(summary->free_clusters == 63);
  CHECK(summary->unknown_clusters == 0);
  CHECK(opened.volume->GetClusterState(64) == ClusterState::OutOfRange);
}

template <Cache::Strategy S>
void CheckAShortBitmapCountsWhatItCovers() {
  const auto opened =
      Support::Open<S>(TreeWithResidentBitmap({0x01, 0x00, 0x00, 0x00}));

  const auto summary = opened.volume->GetSpaceSummary();
  REQUIRE(summary);
  CHECK(summary->total_clusters == 64);
  CHECK(summary->free_clusters == 31);
  CHECK(summary->free_bytes == 31 * Tests::fake_cluster_size);
  CHECK(summary->unknown_clusters == 32);
}

template <Cache::Strategy S>
void CheckAMissingBitmapHasNoSummary() {
  const auto opened =
      Support::Open<S>(Tests::BuildFakeNtfsImageWithFreedFileTree());

  CHECK_FALSE(opened.volume->GetSpaceSummary());
}

template <Cache::Strategy S>
void CheckTheSummaryIsComputedOnce() {
  const auto opened = Support::Open<S>(TreeWithBitmap());

  const auto first = opened.volume->GetSpaceSummary();
  const size_t reads = opened.counter->ReadCount();
  const auto second = opened.volume->GetSpaceSummary();

  REQUIRE(first);
  REQUIRE(second);
  CHECK(second->free_clusters == first->free_clusters);
  CHECK(opened.counter->ReadCount() == reads);
}

template <Cache::Strategy S>
void CheckTheOptionsDoNotChangeTheSummary() {
  const std::vector<BYTE> image = TreeWithBitmap();
  std::optional<ULONGLONG> expected;
  for (const bool include_deleted : {false, true}) {
    const auto opened = Support::Open<S>(
        image, VolumeOptions{.include_deleted = include_deleted});
    const auto summary = opened.volume->GetSpaceSummary();
    REQUIRE(summary);
    if (!expected) {
      expected = summary->free_clusters;
    }
    CHECK(summary->free_clusters == *expected);
  }
}

}  // namespace

TEST_CASE("free space equals the known value", "[alloc][summary]") {
  CheckFreeSpaceIsTheKnownValue<Cache::Strategy::NoCache>();
  CheckFreeSpaceIsTheKnownValue<Cache::Strategy::FullCache>();
}

TEST_CASE("padding bits past the last cluster are not counted",
          "[alloc][summary]") {
  CheckPaddingBitsDoNotCount<Cache::Strategy::NoCache>();
  CheckPaddingBitsDoNotCount<Cache::Strategy::FullCache>();
}

TEST_CASE(
    "a short bitmap gives the free clusters it covers and the rest unknown",
    "[alloc][summary]") {
  CheckAShortBitmapCountsWhatItCovers<Cache::Strategy::NoCache>();
  CheckAShortBitmapCountsWhatItCovers<Cache::Strategy::FullCache>();
}

TEST_CASE("a volume with no bitmap has no free space report",
          "[alloc][summary]") {
  CheckAMissingBitmapHasNoSummary<Cache::Strategy::NoCache>();
  CheckAMissingBitmapHasNoSummary<Cache::Strategy::FullCache>();
}

TEST_CASE("the free space report is computed once", "[alloc][summary]") {
  CheckTheSummaryIsComputedOnce<Cache::Strategy::NoCache>();
  CheckTheSummaryIsComputedOnce<Cache::Strategy::FullCache>();
}

TEST_CASE("include_deleted does not change the free space report",
          "[alloc][summary]") {
  CheckTheOptionsDoNotChangeTheSummary<Cache::Strategy::NoCache>();
  CheckTheOptionsDoNotChangeTheSummary<Cache::Strategy::FullCache>();
}
