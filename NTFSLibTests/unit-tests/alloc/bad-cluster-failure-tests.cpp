#include <ntfs-browser/win-types.h>

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-failure-support.h"
#include "alloc-test-support.h"
#include "data/ntfs-bpb.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::ClusterState;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::LevelSink;
using Support::ScopedSink;
using Support::ThrowingCallback;
using Tests::FakeBadClusterOptions;
using Tests::FakeDataRun;

namespace {

// The message of a bad list that cannot be used at all.
constexpr std::string_view unusable_message = "$BadClus is not usable";

// Clusters the fixture marks allocated, besides the bitmap's own.
const std::vector<FakeDataRun> allocated_runs{
    FakeDataRun{.lcn = 40, .clusters = 2}};

// 45 and 46 are marked allocated in the bitmap, as NTFS does.
const std::vector<Alloc::Extent> bad_marked{{.lcn = 45, .clusters = 2}};

// Cluster where every fixture puts $MFT.
constexpr ULONGLONG fake_mft_lcn = 1;

// The freed tree, a bitmap marking 40, 41 and the bad clusters, and a bad list
// shaped by options.
std::vector<BYTE> Image(std::span<const Alloc::Extent> bad,
                        const FakeBadClusterOptions& options = {}) {
  std::vector<BYTE> image =
      Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(),
                          Support::WithBadRuns(allocated_runs, bad_marked));
  return Support::WithBadClusters(std::move(image), bad, options);
}

// Byte range of MFT record 8 in an image.
struct RecordRange {
  ULONGLONG first;
  ULONGLONG last;
};

RecordRange BadClusRecord() {
  const ULONGLONG first =
      fake_mft_lcn * Tests::fake_cluster_size +
      static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::BadCluster) *
          Tests::fake_file_record_size;
  return {first, first + Tests::fake_file_record_size};
}

template <Cache::Strategy S>
void CheckNothingIsReadBeforeAQuery() {
  const auto opened = Support::Open<S>(Image(bad_marked));
  const auto record = Support::ParsedRecord(
      *opened.volume, static_cast<ULONGLONG>(Tests::mft_tree_docs_idx));

  if constexpr (S == Cache::Strategy::NoCache) {
    const RecordRange range = BadClusRecord();
    CHECK(opened.counter->ReadsInRange(range.first, range.last) == 0);
    CHECK(opened.volume->GetClusterState(45) == ClusterState::Bad);
    CHECK(opened.counter->ReadsInRange(range.first, range.last) > 0);
  } else {
    CHECK(opened.volume->GetClusterState(45) == ClusterState::Bad);
  }
}

template <Cache::Strategy S>
void CheckAnUnusableListFallsBackOnTheBitmap(
    const FakeBadClusterOptions& options, bool record_is_written) {
  const auto sink = std::make_shared<LevelSink>();
  const ScopedSink scoped(sink);
  std::vector<BYTE> image = Image(bad_marked, options);
  if (!record_is_written) {
    // Record 8 left as the base builds it: zero-filled.
    image =
        Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(),
                            Support::WithBadRuns(allocated_runs, bad_marked));
  }
  const auto opened = Support::Open<S>(image);

  CHECK_NOTHROW(opened.volume->GetClusterState(45));
  CHECK(opened.volume->GetBadClusters() == nullptr);
  // The bitmap alone says allocated, as before this feature.
  CHECK(opened.volume->GetClusterState(45) == ClusterState::Allocated);
  CHECK(opened.volume->GetClusterState(47) == ClusterState::Free);
  CHECK(opened.volume->GetSpaceSummary());

  CHECK(sink->Count(unusable_message) == 1);
  CHECK(sink->LevelOf(unusable_message) == Log::Level::Warn);
}

template <Cache::Strategy S>
void CheckAnUnusableListIsNotReadAgain() {
  const auto opened =
      Support::Open<S>(Image(bad_marked, {.omit_bad_stream = true}));
  REQUIRE(opened.volume->GetBadClusters() == nullptr);

  opened.counter->Reset();
  CHECK(opened.volume->GetBadClusters() == nullptr);
  CHECK(opened.volume->GetClusterState(45) == ClusterState::Allocated);
  if constexpr (S == Cache::Strategy::NoCache) {
    const RecordRange range = BadClusRecord();
    CHECK(opened.counter->ReadsInRange(range.first, range.last) == 0);
  }
}

template <Cache::Strategy S>
void CheckTheLevelFollowsRecoverErrors() {
  const auto sink = std::make_shared<LevelSink>();
  const ScopedSink scoped(sink);
  const auto opened = Support::Open<S>(
      Image(bad_marked, {.omit_bad_stream = true}),
      VolumeOptions{.include_deleted = true, .recover_errors = true});

  CHECK(opened.volume->GetBadClusters() == nullptr);
  CHECK(sink->Count(unusable_message) == 1);
  CHECK(sink->LevelOf(unusable_message) == Log::Level::Info);
}

template <Cache::Strategy S>
void CheckACutRunList() {
  const std::vector<Alloc::Extent> two{{.lcn = 45, .clusters = 2},
                                       {.lcn = 52, .clusters = 1}};
  const std::vector<BYTE> image = Image(two, {.cut_run_list = true});

  const auto strict = Support::Open<S>(image);
  CHECK(strict.volume->GetBadClusters() == nullptr);

  const auto recovering = Support::Open<S>(
      image, VolumeOptions{.include_deleted = true, .recover_errors = true});
  const Alloc::BadClusters* list = recovering.volume->GetBadClusters();
  REQUIRE(list != nullptr);
  REQUIRE(list->extents.size() == 1);
  CHECK(list->extents[0].lcn == 45);
  CHECK(list->extents[0].clusters == 2);
  CHECK_FALSE(list->truncated);
  CHECK(recovering.volume->GetClusterState(52) == ClusterState::Free);
}

template <Cache::Strategy S>
void CheckAMissingBitmapKeepsTheBadClusters() {
  // Record 6 is zero-filled: there is no bitmap, but the list is whole.
  const std::vector<BYTE> image = Support::WithBadClusters(
      Tests::BuildFakeNtfsImageWithFreedFileTree(), bad_marked);
  const auto opened = Support::Open<S>(image);

  REQUIRE(opened.volume->GetBadClusters() != nullptr);
  CHECK(opened.volume->GetClusterState(45) == ClusterState::Bad);
  CHECK(opened.volume->GetRangeState(44, 4) == ClusterState::Bad);
  CHECK(opened.volume->GetClusterState(47) == ClusterState::Unknown);
  CHECK_FALSE(opened.volume->GetSpaceSummary());
}

template <Cache::Strategy S>
void CheckNeitherFileUsable() {
  const auto opened =
      Support::Open<S>(Tests::BuildFakeNtfsImageWithFreedFileTree());

  CHECK(opened.volume->GetBadClusters() == nullptr);
  CHECK(opened.volume->GetClusterState(45) == ClusterState::Unknown);
  CHECK(opened.volume->GetRangeState(0, 10) == ClusterState::Unknown);
  CHECK_FALSE(opened.volume->GetSpaceSummary());
}

template <Cache::Strategy S>
void CheckOddStreamFlagsChangeNothing() {
  // Compressed, encrypted, both, and none: the list comes from the run list.
  for (const WORD flags : {WORD{0x0001}, WORD{0x4000}, WORD{0x8001}, WORD{0}}) {
    const auto opened =
        Support::Open<S>(Image(bad_marked, {.attr_flags = flags}));

    const Alloc::BadClusters* list = opened.volume->GetBadClusters();
    REQUIRE(list != nullptr);
    REQUIRE(list->extents.size() == 1);
    CHECK(list->extents[0].lcn == 45);
    CHECK(opened.volume->GetClusterState(46) == ClusterState::Bad);
  }
}

template <Cache::Strategy S>
void CheckAForgedSizeChangesNothing() {
  for (const ULONGLONG size :
       {ULONGLONG{0}, ULONGLONG{123}, ULONGLONG{1} << 50}) {
    const auto opened =
        Support::Open<S>(Image(bad_marked, {.forged_size = size}));

    const Alloc::BadClusters* list = opened.volume->GetBadClusters();
    REQUIRE(list != nullptr);
    REQUIRE(list->extents.size() == 1);
    CHECK(opened.volume->GetClusterState(45) == ClusterState::Bad);
  }
}

template <Cache::Strategy S>
void CheckAFreedRecordIsStillRead() {
  for (const bool include_deleted : {false, true}) {
    const auto opened =
        Support::Open<S>(Image(bad_marked, {.record_not_in_use = true}),
                         VolumeOptions{.include_deleted = include_deleted});

    REQUIRE(opened.volume->GetBadClusters() != nullptr);
    CHECK(opened.volume->GetClusterState(45) == ClusterState::Bad);
  }
}

template <Cache::Strategy S>
void CheckAnImageSmallerThanTheVolume() {
  std::vector<BYTE> image = Image({});
  const ULONGLONG real_clusters = image.size() / Tests::fake_cluster_size;
  // The boot sector claims 1000 more clusters than the image holds.
  auto& bpb =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::NtfsBpb*>(image.data());
  bpb.total_sectors += 1000ULL * Tests::fake_sectors_per_cluster;
  const std::vector<Alloc::Extent> tail{
      {.lcn = real_clusters - 2, .clusters = 10},
      {.lcn = real_clusters + 5, .clusters = 3}};
  image = Support::WithBadClusters(std::move(image), tail);

  const auto opened = Support::Open<S>(image);

  const Alloc::BadClusters* list = opened.volume->GetBadClusters();
  REQUIRE(list != nullptr);
  REQUIRE(list->extents.size() == 1);
  CHECK(list->extents[0].lcn == real_clusters - 2);
  CHECK(list->extents[0].clusters == 2);
  CHECK(opened.volume->GetClusterState(real_clusters) ==
        ClusterState::OutOfRange);
}

template <Cache::Strategy S>
void CheckAThrowingCallbackIsContained() {
  const auto opened = Support::Open<S>(Image(bad_marked));
  REQUIRE(opened.volume->InstallAttrRawCB(Attr::Type::Data, &ThrowingCallback));

  CHECK_NOTHROW(opened.volume->GetBadClusters());
  CHECK(opened.volume->GetBadClusters() == nullptr);
  CHECK_NOTHROW(opened.volume->GetClusterState(45));
  CHECK(opened.volume->GetClusterState(45) == ClusterState::Unknown);
  CHECK(opened.volume->GetRangeState(44, 4) == ClusterState::Unknown);
  CHECK_FALSE(opened.volume->GetSpaceSummary());
}

}  // namespace

TEST_CASE("nothing reads the bad list before a query needs it",
          "[alloc][bad-failure]") {
  CheckNothingIsReadBeforeAQuery<Cache::Strategy::NoCache>();
  CheckNothingIsReadBeforeAQuery<Cache::Strategy::FullCache>();
}

TEST_CASE(
    "a bad list with no $Bad stream falls back on the bitmap, logged once",
    "[alloc][bad-failure]") {
  CheckAnUnusableListFallsBackOnTheBitmap<Cache::Strategy::NoCache>(
      {.omit_bad_stream = true}, true);
  CheckAnUnusableListFallsBackOnTheBitmap<Cache::Strategy::FullCache>(
      {.omit_bad_stream = true}, true);
}

TEST_CASE("a zero-filled record 8 falls back on the bitmap, logged once",
          "[alloc][bad-failure]") {
  CheckAnUnusableListFallsBackOnTheBitmap<Cache::Strategy::NoCache>({}, false);
  CheckAnUnusableListFallsBackOnTheBitmap<Cache::Strategy::FullCache>({},
                                                                      false);
}

TEST_CASE("an unusable bad list is not read a second time",
          "[alloc][bad-failure]") {
  CheckAnUnusableListIsNotReadAgain<Cache::Strategy::NoCache>();
  CheckAnUnusableListIsNotReadAgain<Cache::Strategy::FullCache>();
}

TEST_CASE("the unusable bad list is logged as info when recovering",
          "[alloc][bad-failure]") {
  CheckTheLevelFollowsRecoverErrors<Cache::Strategy::NoCache>();
  CheckTheLevelFollowsRecoverErrors<Cache::Strategy::FullCache>();
}

TEST_CASE("a cut run list is rejected when strict and kept when recovering",
          "[alloc][bad-failure]") {
  CheckACutRunList<Cache::Strategy::NoCache>();
  CheckACutRunList<Cache::Strategy::FullCache>();
}

TEST_CASE("without a bitmap the bad clusters stay bad and the rest unknown",
          "[alloc][bad-failure]") {
  CheckAMissingBitmapKeepsTheBadClusters<Cache::Strategy::NoCache>();
  CheckAMissingBitmapKeepsTheBadClusters<Cache::Strategy::FullCache>();
}

TEST_CASE("with neither file usable every answer is unknown",
          "[alloc][bad-failure]") {
  CheckNeitherFileUsable<Cache::Strategy::NoCache>();
  CheckNeitherFileUsable<Cache::Strategy::FullCache>();
}

TEST_CASE("odd attribute flags on $Bad change nothing",
          "[alloc][bad-failure]") {
  CheckOddStreamFlagsChangeNothing<Cache::Strategy::NoCache>();
  CheckOddStreamFlagsChangeNothing<Cache::Strategy::FullCache>();
}

TEST_CASE("a forged size of $Bad changes nothing", "[alloc][bad-failure]") {
  CheckAForgedSizeChangesNothing<Cache::Strategy::NoCache>();
  CheckAForgedSizeChangesNothing<Cache::Strategy::FullCache>();
}

TEST_CASE("a freed record 8 is read whatever include_deleted says",
          "[alloc][bad-failure]") {
  CheckAFreedRecordIsStillRead<Cache::Strategy::NoCache>();
  CheckAFreedRecordIsStillRead<Cache::Strategy::FullCache>();
}

TEST_CASE("a bad extent past the end of the image is cut",
          "[alloc][bad-failure]") {
  CheckAnImageSmallerThanTheVolume<Cache::Strategy::NoCache>();
  CheckAnImageSmallerThanTheVolume<Cache::Strategy::FullCache>();
}

TEST_CASE("a throwing callback is contained while the list loads",
          "[alloc][bad-failure]") {
  CheckAThrowingCallbackIsContained<Cache::Strategy::NoCache>();
  CheckAThrowingCallbackIsContained<Cache::Strategy::FullCache>();
}
