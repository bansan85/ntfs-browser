#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "catch2/matchers/catch_matchers.hpp"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::ReadStale;
using Support::stale_size;
using Tests::FakeDataRun;

namespace {

// What the check says, as a substring of its message.
constexpr const char* free_message =
    "clusters of a data stream are free in $Bitmap";

const VolumeOptions strict{};
const VolumeOptions recovering{.recover_errors = true};

// A live stale file over four clusters, and a bitmap that marks allocated_runs.
std::vector<BYTE> LiveImage(std::span<const FakeDataRun> allocated_runs,
                            bool in_use = true) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
      stale_size, {FakeDataRun{.lcn = 40, .clusters = 4}});
  Support::FillStaleClusters(image, 13);
  Tests::SetRecordInUse(image, Tests::mft_tree_stale_child_idx, in_use);
  return Support::WithBitmap(std::move(image), allocated_runs);
}

template <Cache::Strategy S>
void CheckAFreeClusterIsLogged() {
  const auto strict_opened = Support::OpenVolume<S>(LiveImage({}), strict);
  const std::vector<BYTE> expected = ReadStale(strict_opened);

  const auto opened = Support::OpenVolume<S>(LiveImage({}), recovering);
  (void)Tests::TakeCapturedLog();

  CHECK(ReadStale(opened) == expected);
  CHECK_THAT(Tests::TakeCapturedLog(), ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckAPartlyFreeRunIsLogged() {
  const std::array<FakeDataRun, 1> one_used{
      FakeDataRun{.lcn = 41, .clusters = 1}};
  const std::string log =
      Support::LogOfReadingStale<S>(LiveImage(one_used), recovering);

  CHECK_THAT(log, ContainsSubstring(free_message));
  CHECK_THAT(log, ContainsSubstring("3 clusters"));
}

template <Cache::Strategy S>
void CheckAHealthyFileIsNotLogged() {
  const std::array<FakeDataRun, 1> all_used{
      FakeDataRun{.lcn = 40, .clusters = 4}};
  const std::string log =
      Support::LogOfReadingStale<S>(LiveImage(all_used), recovering);

  CHECK_THAT(log, !ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckStrictModeReadsNoBitmap() {
  const std::vector<BYTE> image = LiveImage({});
  const auto opened = Support::OpenVolume<S>(image, strict);
  (void)Tests::TakeCapturedLog();
  opened.counter->Reset();

  (void)ReadStale(opened);

  CHECK_THAT(Tests::TakeCapturedLog(), !ContainsSubstring(free_message));
  CHECK(opened.counter->ReadsInRange(Support::bitmap_data_offset,
                                     image.size()) == 0);
}

template <Cache::Strategy S>
void CheckTheCheckRunsOnce() {
  const auto opened = Support::OpenVolume<S>(LiveImage({}), recovering);
  const auto record = Support::ParsedStale(opened);
  std::vector<BYTE> out(stale_size);
  const auto* const stream = record->FindStream({});
  (void)Tests::TakeCapturedLog();

  REQUIRE(stream->ReadData(0, out));
  const std::string first = Tests::TakeCapturedLog();
  REQUIRE(stream->ReadData(0, out));
  const std::string second = Tests::TakeCapturedLog();

  CHECK_THAT(first, ContainsSubstring(free_message));
  CHECK_THAT(second, !ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckADeletedFileIsNotChecked() {
  const std::string log = Support::LogOfReadingStale<S>(
      LiveImage({}, /*in_use=*/false),
      VolumeOptions{.include_deleted = true, .recover_errors = true});

  CHECK_THAT(log, !ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckTheVolumesOwnMetadataIsNotChecked() {
  const std::vector<BYTE> image = LiveImage({});
  const auto opened = Support::OpenVolume<S>(image, recovering);
  (void)Tests::TakeCapturedLog();
  opened.counter->Reset();

  const auto mft = Support::ParsedRecord(*opened.volume,
                                         static_cast<ULONGLONG>(Mft::Idx::Mft));
  std::vector<BYTE> mft_bytes(Tests::fake_cluster_size);
  CHECK(mft->FindStream({})->ReadData(0, mft_bytes));

  const auto bitmap = Support::ParsedRecord(
      *opened.volume, static_cast<ULONGLONG>(Mft::Idx::Bitmap));
  std::vector<BYTE> bitmap_bytes(8);
  CHECK(bitmap->FindStream({})->ReadData(0, bitmap_bytes));

  CHECK_THAT(Tests::TakeCapturedLog(), !ContainsSubstring(free_message));
}

// A live stale file in two fragments of two clusters, at LCN 40 and 50.
std::vector<BYTE> FragmentedLiveImage(std::span<const FakeDataRun> allocated,
                                      std::span<const Alloc::Extent> bad) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
      stale_size, {FakeDataRun{.lcn = 40, .clusters = 2},
                   FakeDataRun{.lcn = 50, .clusters = 2}});
  Tests::SetRecordInUse(image, Tests::mft_tree_stale_child_idx, true);
  image = Support::WithBitmap(std::move(image), allocated);
  if (!bad.empty()) {
    image = Support::WithBadClusters(std::move(image), bad);
  }
  return image;
}

template <Cache::Strategy S>
void CheckEveryFragmentIsCounted() {
  SECTION("free clusters of two fragments") {
    const std::string log =
        Support::LogOfReadingStale<S>(FragmentedLiveImage({}, {}), recovering);

    CHECK_THAT(log, ContainsSubstring("4 clusters of a data stream are free in "
                                      "$Bitmap, from the run at LCN 40"));
  }

  SECTION("bad clusters of two fragments") {
    const std::vector<Alloc::Extent> bad{{.lcn = 40, .clusters = 2},
                                         {.lcn = 50, .clusters = 2}};
    const std::string log = Support::LogOfReadingStale<S>(
        FragmentedLiveImage(Support::WithBadRuns({}, bad), bad), recovering);

    CHECK_THAT(log,
               ContainsSubstring("4 clusters of a data stream are marked bad "
                                 "in $BadClus, from the run at LCN 40"));
  }
}

}  // namespace

TEST_CASE("a live file on free clusters is logged in recover mode",
          "[alloc][live-check]") {
  CheckAFreeClusterIsLogged<Cache::Strategy::NoCache>();
  CheckAFreeClusterIsLogged<Cache::Strategy::FullCache>();
}

TEST_CASE("a live run that is only partly free is logged",
          "[alloc][live-check]") {
  CheckAPartlyFreeRunIsLogged<Cache::Strategy::NoCache>();
  CheckAPartlyFreeRunIsLogged<Cache::Strategy::FullCache>();
}

TEST_CASE("a healthy live file is not logged", "[alloc][live-check]") {
  CheckAHealthyFileIsNotLogged<Cache::Strategy::NoCache>();
  CheckAHealthyFileIsNotLogged<Cache::Strategy::FullCache>();
}

TEST_CASE("strict mode reads no bitmap when it reads a live file",
          "[alloc][live-check]") {
  CheckStrictModeReadsNoBitmap<Cache::Strategy::NoCache>();
  CheckStrictModeReadsNoBitmap<Cache::Strategy::FullCache>();
}

TEST_CASE("the free cluster check runs once per stream",
          "[alloc][live-check]") {
  CheckTheCheckRunsOnce<Cache::Strategy::NoCache>();
  CheckTheCheckRunsOnce<Cache::Strategy::FullCache>();
}

TEST_CASE("a deleted file is not checked", "[alloc][live-check]") {
  CheckADeletedFileIsNotChecked<Cache::Strategy::NoCache>();
  CheckADeletedFileIsNotChecked<Cache::Strategy::FullCache>();
}

TEST_CASE("the volume's own metadata is not checked", "[alloc][live-check]") {
  CheckTheVolumesOwnMetadataIsNotChecked<Cache::Strategy::NoCache>();
  CheckTheVolumesOwnMetadataIsNotChecked<Cache::Strategy::FullCache>();
}

TEMPLATE_TEST_CASE_SIG("a live file counts the clusters of every fragment",
                       "[alloc][live-check]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckEveryFragmentIsCounted<S>();
}
