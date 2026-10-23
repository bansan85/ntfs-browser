#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::DataState;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::stale_size;
using Tests::FakeDataRun;

namespace {

// The stale file's one run: four clusters at LCN 40.
const std::vector<FakeDataRun> stale_runs{
    FakeDataRun{.lcn = 40, .clusters = 4}};

// The freed stale file with its run, and a bitmap that marks allocated_runs.
std::vector<BYTE> StaleImage(std::span<const FakeDataRun> allocated_runs,
                             const std::vector<FakeDataRun>& runs = stale_runs,
                             ULONGLONG real_size = stale_size) {
  return Support::WithBitmap(
      Tests::BuildFakeNtfsImageWithFreedNonResidentFile(real_size, runs),
      allocated_runs);
}

template <Cache::Strategy S>
std::optional<DataState>
    StateOfStale(const std::vector<BYTE>& image,
                 const VolumeOptions& options = Support::DeletedOptions()) {
  const auto opened = Support::OpenVolume<S>(image, options);
  return Support::ParsedStale(opened)->GetDataState();
}

template <Cache::Strategy S>
void CheckFreeClustersAreIntact() {
  CHECK(StateOfStale<S>(StaleImage({})) == DataState::Intact);
}

template <Cache::Strategy S>
void CheckAnAllocatedClusterIsReallocated() {
  std::vector<BYTE> without =
      Tests::BuildFakeNtfsImageWithFreedNonResidentFile(stale_size, stale_runs);
  const std::array<FakeDataRun, 1> reused{
      FakeDataRun{.lcn = 42, .clusters = 1}};
  std::vector<BYTE> with = StaleImage(reused);

  // Fill the file's clusters, so a read returns something to compare.
  for (std::vector<BYTE>* image : {&without, &with}) {
    Support::FillStaleClusters(*image, 7);
  }

  const auto bytes_of = [](const std::vector<BYTE>& image) {
    return Support::ReadStale(Support::OpenVolume<S>(image));
  };

  CHECK(StateOfStale<S>(with) == DataState::Reallocated);
  CHECK(bytes_of(with) == bytes_of(without));
}

template <Cache::Strategy S>
void CheckAHoleIsIgnored() {
  const std::vector<FakeDataRun> runs{FakeDataRun{.lcn = 40, .clusters = 2},
                                      FakeDataRun{.clusters = 3},
                                      FakeDataRun{.lcn = 50, .clusters = 2}};
  CHECK(StateOfStale<S>(StaleImage({}, runs, 7 * Tests::fake_cluster_size)) ==
        DataState::Intact);

  const std::array<FakeDataRun, 1> reused{
      FakeDataRun{.lcn = 51, .clusters = 1}};
  CHECK(
      StateOfStale<S>(StaleImage(reused, runs, 7 * Tests::fake_cluster_size)) ==
      DataState::Reallocated);
}

template <Cache::Strategy S>
void CheckResidentDataIsIntact() {
  const std::array<FakeDataRun, 1> reused{
      FakeDataRun{.lcn = 40, .clusters = 4}};
  const std::vector<BYTE> image =
      Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(), reused);
  const auto opened = Support::OpenVolume<S>(image);
  const auto record =
      Support::ParsedRecord(*opened.volume, Tests::mft_tree_deleted_file_idx);

  CHECK(record->GetDataState() == DataState::Intact);
}

template <Cache::Strategy S>
void CheckALiveFileIsIntactWithoutTheBitmap() {
  std::vector<BYTE> image = StaleImage(stale_runs);
  Tests::SetRecordInUse(image, Tests::mft_tree_stale_child_idx, true);
  const auto opened = Support::OpenVolume<S>(image);
  const auto record = Support::ParsedStale(opened);
  opened.counter->Reset();

  CHECK(record->GetDataState() == DataState::Intact);
  CHECK(opened.counter->ReadsInRange(Support::bitmap_data_offset,
                                     image.size()) == 0);
}

template <Cache::Strategy S>
void CheckAMissingBitmapIsUnknown() {
  // Record 6 is zero-filled in the freed tree: there is no bitmap.
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithFreedNonResidentFile(stale_size, stale_runs);

  CHECK(StateOfStale<S>(image) == DataState::Unknown);
}

template <Cache::Strategy S>
void CheckAnExtentPastTheEndOfTheVolume() {
  // The second run lies far past the 64 clusters of the base image.
  const std::vector<FakeDataRun> runs{FakeDataRun{.lcn = 40, .clusters = 2},
                                      FakeDataRun{.lcn = 5000, .clusters = 2}};
  const std::array<FakeDataRun, 1> reused{
      FakeDataRun{.lcn = 41, .clusters = 1}};

  CHECK(
      StateOfStale<S>(StaleImage(reused, runs, 4 * Tests::fake_cluster_size)) ==
      DataState::Reallocated);
  CHECK(StateOfStale<S>(StaleImage({}, runs, 4 * Tests::fake_cluster_size)) ==
        DataState::Unknown);
}

template <Cache::Strategy S>
void CheckAStreamThatDoesNotExist() {
  const auto opened = Support::OpenVolume<S>(StaleImage({}));
  const auto record = Support::ParsedStale(opened);

  CHECK_FALSE(record->GetDataState(L"no-such-stream"));
}

template <Cache::Strategy S>
void CheckACutRunListInRecoverMode() {
  // Declared: 6 clusters. The list that parses maps the first 5, at LCN 30
  // (3 clusters) and LCN 40 (2 clusters).
  const std::vector<FakeDataRun> declared{
      FakeDataRun{.lcn = 30, .clusters = 3},
      FakeDataRun{.lcn = 40, .clusters = 2},
      FakeDataRun{.lcn = 50, .clusters = 1}};
  const std::vector<BYTE> raw{0x11, 0x03, 0x1E, 0x11, 0x02, 0x0A, 0x1F, 0x00};
  const auto image_for = [&](std::span<const FakeDataRun> allocated) {
    std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
    Tests::PutFreedNonResidentRecord(image, Tests::mft_tree_stale_child_idx,
                                     {.real_size = 6 * Tests::fake_cluster_size,
                                      .runs = declared,
                                      .raw_runs = raw});
    return Support::WithBitmap(std::move(image), allocated);
  };
  const VolumeOptions recover{.include_deleted = true, .recover_errors = true};

  const std::array<FakeDataRun, 1> reused{
      FakeDataRun{.lcn = 41, .clusters = 1}};
  CHECK(StateOfStale<S>(image_for(reused), recover) == DataState::Reallocated);
  CHECK(StateOfStale<S>(image_for({}), recover) == DataState::Unknown);
}

}  // namespace

TEST_CASE("a deleted stream on free clusters is intact",
          "[alloc][data-state]") {
  CheckFreeClustersAreIntact<Cache::Strategy::NoCache>();
  CheckFreeClustersAreIntact<Cache::Strategy::FullCache>();
}

TEST_CASE("a deleted stream on a reused cluster is reallocated and readable",
          "[alloc][data-state]") {
  CheckAnAllocatedClusterIsReallocated<Cache::Strategy::NoCache>();
  CheckAnAllocatedClusterIsReallocated<Cache::Strategy::FullCache>();
}

TEST_CASE("a hole in a deleted stream owns no cluster", "[alloc][data-state]") {
  CheckAHoleIsIgnored<Cache::Strategy::NoCache>();
  CheckAHoleIsIgnored<Cache::Strategy::FullCache>();
}

TEST_CASE("a deleted stream with resident data is intact",
          "[alloc][data-state]") {
  CheckResidentDataIsIntact<Cache::Strategy::NoCache>();
  CheckResidentDataIsIntact<Cache::Strategy::FullCache>();
}

TEST_CASE("a live file is intact and the bitmap is not read",
          "[alloc][data-state]") {
  CheckALiveFileIsIntactWithoutTheBitmap<Cache::Strategy::NoCache>();
  CheckALiveFileIsIntactWithoutTheBitmap<Cache::Strategy::FullCache>();
}

TEST_CASE("a deleted stream is unknown when there is no bitmap",
          "[alloc][data-state]") {
  CheckAMissingBitmapIsUnknown<Cache::Strategy::NoCache>();
  CheckAMissingBitmapIsUnknown<Cache::Strategy::FullCache>();
}

TEST_CASE("an extent past the end of the volume does not hide a reused one",
          "[alloc][data-state]") {
  CheckAnExtentPastTheEndOfTheVolume<Cache::Strategy::NoCache>();
  CheckAnExtentPastTheEndOfTheVolume<Cache::Strategy::FullCache>();
}

TEST_CASE("a stream that does not exist has no data state",
          "[alloc][data-state]") {
  CheckAStreamThatDoesNotExist<Cache::Strategy::NoCache>();
  CheckAStreamThatDoesNotExist<Cache::Strategy::FullCache>();
}

TEST_CASE("a run list cut in recover mode keeps what it can judge",
          "[alloc][data-state]") {
  CheckACutRunListInRecoverMode<Cache::Strategy::NoCache>();
  CheckACutRunListInRecoverMode<Cache::Strategy::FullCache>();
}
