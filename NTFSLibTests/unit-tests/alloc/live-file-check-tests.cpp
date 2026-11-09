#include <array>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "alloc/types.h"
#include "cache/strategy.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "volume-options.h"
#include "win-types.h"

using Catch::Matchers::ContainsSubstring;
namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestAlloc = NTFS_BROWSER_NAMESPACE::Test::Alloc;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using TestAlloc::ReadStale;
using TestAlloc::stale_size;
using TestFakeImage::FakeDataRun;

namespace {

// What the check says, as a substring of its message.
constexpr const char* free_message =
    "clusters of a data stream are free in $Bitmap";

const VolumeOptions strict{};
const VolumeOptions recovering{.recover_errors = true};

// A live stale file over four clusters, and a bitmap that marks allocated_runs.
std::vector<BYTE> LiveImage(std::span<const FakeDataRun> allocated_runs,
                            bool in_use = true) {
  std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithFreedNonResidentFile(
          stale_size, {FakeDataRun{.lcn = 40, .clusters = 4}});
  TestAlloc::FillStaleClusters(image, 13);
  TestFakeImage::SetRecordInUse(image, TestFakeImage::mft_tree_stale_child_idx,
                                in_use);
  return TestAlloc::WithBitmap(std::move(image), allocated_runs);
}

template <Cache::Strategy S>
void CheckAFreeClusterIsLogged() {
  const auto strict_opened = TestAlloc::OpenVolume<S>(LiveImage({}), strict);
  const std::vector<BYTE> expected = ReadStale(strict_opened);

  const auto opened = TestAlloc::OpenVolume<S>(LiveImage({}), recovering);
  (void)TestSupport::TakeCapturedLog();

  CHECK(ReadStale(opened) == expected);
  CHECK_THAT(TestSupport::TakeCapturedLog(), ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckAPartlyFreeRunIsLogged() {
  const std::array<FakeDataRun, 1> one_used{
      FakeDataRun{.lcn = 41, .clusters = 1}};
  const std::string log =
      TestAlloc::LogOfReadingStale<S>(LiveImage(one_used), recovering);

  CHECK_THAT(log, ContainsSubstring(free_message));
  CHECK_THAT(log, ContainsSubstring("3 clusters"));
}

template <Cache::Strategy S>
void CheckAHealthyFileIsNotLogged() {
  const std::array<FakeDataRun, 1> all_used{
      FakeDataRun{.lcn = 40, .clusters = 4}};
  const std::string log =
      TestAlloc::LogOfReadingStale<S>(LiveImage(all_used), recovering);

  CHECK_THAT(log, !ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckStrictModeReadsNoBitmap() {
  const std::vector<BYTE> image = LiveImage({});
  const auto opened = TestAlloc::OpenVolume<S>(image, strict);
  (void)TestSupport::TakeCapturedLog();
  opened.counter->Reset();

  (void)ReadStale(opened);

  CHECK_THAT(TestSupport::TakeCapturedLog(), !ContainsSubstring(free_message));
  CHECK(opened.counter->ReadsInRange(TestAlloc::bitmap_data_offset,
                                     image.size()) == 0);
}

template <Cache::Strategy S>
void CheckTheCheckRunsOnce() {
  const auto opened = TestAlloc::OpenVolume<S>(LiveImage({}), recovering);
  const auto record = TestAlloc::ParsedStale(opened);
  std::vector<BYTE> out(stale_size);
  const auto* const stream = record->FindStream({});
  (void)TestSupport::TakeCapturedLog();

  REQUIRE(stream->ReadData(0, out));
  const std::string first = TestSupport::TakeCapturedLog();
  REQUIRE(stream->ReadData(0, out));
  const std::string second = TestSupport::TakeCapturedLog();

  CHECK_THAT(first, ContainsSubstring(free_message));
  CHECK_THAT(second, !ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckADeletedFileIsNotChecked() {
  const std::string log = TestAlloc::LogOfReadingStale<S>(
      LiveImage({}, /*in_use=*/false),
      VolumeOptions{.include_deleted = true, .recover_errors = true});

  CHECK_THAT(log, !ContainsSubstring(free_message));
}

template <Cache::Strategy S>
void CheckTheVolumesOwnMetadataIsNotChecked() {
  const std::vector<BYTE> image = LiveImage({});
  const auto opened = TestAlloc::OpenVolume<S>(image, recovering);
  (void)TestSupport::TakeCapturedLog();
  opened.counter->Reset();

  const auto mft = TestAlloc::ParsedRecord(
      *opened.volume, static_cast<ULONGLONG>(Mft::Idx::Mft));
  std::vector<BYTE> mft_bytes(TestFakeImage::fake_cluster_size);
  CHECK(mft->FindStream({})->ReadData(0, mft_bytes));

  const auto bitmap = TestAlloc::ParsedRecord(
      *opened.volume, static_cast<ULONGLONG>(Mft::Idx::Bitmap));
  std::vector<BYTE> bitmap_bytes(8);
  CHECK(bitmap->FindStream({})->ReadData(0, bitmap_bytes));

  CHECK_THAT(TestSupport::TakeCapturedLog(), !ContainsSubstring(free_message));
}

// A live stale file in two fragments of two clusters, at LCN 40 and 50.
std::vector<BYTE> FragmentedLiveImage(std::span<const FakeDataRun> allocated,
                                      std::span<const Alloc::Extent> bad) {
  std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithFreedNonResidentFile(
          stale_size, {FakeDataRun{.lcn = 40, .clusters = 2},
                       FakeDataRun{.lcn = 50, .clusters = 2}});
  TestFakeImage::SetRecordInUse(image, TestFakeImage::mft_tree_stale_child_idx,
                                true);
  image = TestAlloc::WithBitmap(std::move(image), allocated);
  if (!bad.empty()) {
    image = TestAlloc::WithBadClusters(std::move(image), bad);
  }
  return image;
}

template <Cache::Strategy S>
void CheckEveryFragmentIsCounted() {
  SECTION("free clusters of two fragments") {
    const std::string log = TestAlloc::LogOfReadingStale<S>(
        FragmentedLiveImage({}, {}), recovering);

    CHECK_THAT(log, ContainsSubstring("4 clusters of a data stream are free in "
                                      "$Bitmap, from the run at LCN 40"));
  }

  SECTION("bad clusters of two fragments") {
    const std::vector<Alloc::Extent> bad{{.lcn = 40, .clusters = 2},
                                         {.lcn = 50, .clusters = 2}};
    const std::string log = TestAlloc::LogOfReadingStale<S>(
        FragmentedLiveImage(TestAlloc::WithBadRuns({}, bad), bad), recovering);

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
