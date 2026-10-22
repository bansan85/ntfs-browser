#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "fake-ntfs-image.h"
#include "test-log-sink.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::DataState;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeBadClusterOptions;
using Tests::FakeDataRun;

namespace {

// Bytes of the stale stream: four clusters.
constexpr ULONGLONG stale_size = 4 * Tests::fake_cluster_size;

// The stale file's one run: four clusters at LCN 40.
const std::vector<FakeDataRun> stale_runs{
    FakeDataRun{.lcn = 40, .clusters = 4}};

// One bad cluster inside the stale file's run.
const std::vector<Alloc::Extent> one_bad{{.lcn = 41, .clusters = 1}};

// The freed stale file on stale_runs (or runs), a bitmap that marks allocated
// plus the bad clusters named in marked (as NTFS does), and a bad list.
std::vector<BYTE> StaleImage(std::span<const Alloc::Extent> bad,
                             std::span<const Alloc::Extent> marked,
                             std::span<const FakeDataRun> allocated = {},
                             const std::vector<FakeDataRun>& runs = stale_runs,
                             ULONGLONG real_size = stale_size,
                             const FakeBadClusterOptions& options = {}) {
  std::vector<FakeDataRun> in_bitmap(allocated.begin(), allocated.end());
  std::vector<BYTE> image = Support::WithBitmap(
      Tests::BuildFakeNtfsImageWithFreedNonResidentFile(real_size, runs),
      Support::WithBadRuns(std::move(in_bitmap), marked));
  return Support::WithBadClusters(std::move(image), bad, options);
}

template <Cache::Strategy S>
std::optional<DataState>
    StateOfStale(const std::vector<BYTE>& image,
                 const VolumeOptions& options = Support::DeletedOptions()) {
  const auto opened = Support::Open<S>(image, options);
  const auto record =
      Support::ParsedRecord(*opened.volume, Tests::mft_tree_stale_child_idx);
  return record->GetDataState();
}

template <Cache::Strategy S>
void CheckABadClusterIsBadNotReallocated() {
  // The bitmap marks the bad cluster allocated, as NTFS does.
  CHECK(StateOfStale<S>(StaleImage(one_bad, one_bad)) == DataState::Bad);
  // A bad cluster whose bit is clear is bad all the same.
  CHECK(StateOfStale<S>(StaleImage(one_bad, {})) == DataState::Bad);
}

template <Cache::Strategy S>
void CheckReallocatedOutranksBad() {
  const std::array<FakeDataRun, 1> other_file{
      FakeDataRun{.lcn = 43, .clusters = 1}};

  CHECK(StateOfStale<S>(StaleImage(one_bad, one_bad, other_file)) ==
        DataState::Reallocated);
}

template <Cache::Strategy S>
void CheckBadOutranksAnExtentPastTheVolume() {
  // The second run lies far past the 64 clusters of the base image.
  const std::vector<FakeDataRun> runs{FakeDataRun{.lcn = 40, .clusters = 2},
                                      FakeDataRun{.lcn = 5000, .clusters = 2}};

  CHECK(StateOfStale<S>(StaleImage(one_bad, one_bad, {}, runs,
                                   4 * Tests::fake_cluster_size)) ==
        DataState::Bad);
}

template <Cache::Strategy S>
void CheckNoBadClusterIsIntact() {
  CHECK(StateOfStale<S>(StaleImage({}, {})) == DataState::Intact);
}

template <Cache::Strategy S>
void CheckReadingABadStreamIsUnchanged() {
  std::vector<BYTE> without = Support::WithBitmap(
      Tests::BuildFakeNtfsImageWithFreedNonResidentFile(stale_size, stale_runs),
      {});
  std::vector<BYTE> with = StaleImage(one_bad, one_bad);
  for (std::vector<BYTE>* image : {&without, &with}) {
    for (size_t i = 0; i < stale_size; i++) {
      image->at(40 * Tests::fake_cluster_size + i) = static_cast<BYTE>(i * 7);
    }
  }

  const auto bytes_of = [](const std::vector<BYTE>& image) {
    const auto opened = Support::Open<S>(image);
    const auto record =
        Support::ParsedRecord(*opened.volume, Tests::mft_tree_stale_child_idx);
    opened.counter->Reset();
    std::vector<BYTE> out(stale_size);
    const auto read = record->FindStream({})->ReadData(0, out);
    REQUIRE(read);
    REQUIRE(*read == stale_size);
    // The read is issued on the bad cluster 41, as on any other. A FullCache
    // volume has the whole 64 KiB block in memory already, so only NoCache
    // goes to the reader again.
    if constexpr (S == Cache::Strategy::NoCache) {
      CHECK(opened.counter->ReadsInRange(41 * Tests::fake_cluster_size,
                                         42 * Tests::fake_cluster_size) > 0);
    }
    return out;
  };

  CHECK(StateOfStale<S>(with) == DataState::Bad);
  CHECK(bytes_of(with) == bytes_of(without));
}

template <Cache::Strategy S>
void CheckALiveFileIsIntactWithoutAnyRead() {
  std::vector<BYTE> image = StaleImage(one_bad, one_bad);
  Tests::SetRecordInUse(image, Tests::mft_tree_stale_child_idx, true);
  const auto opened = Support::Open<S>(image);
  const auto record =
      Support::ParsedRecord(*opened.volume, Tests::mft_tree_stale_child_idx);
  opened.counter->Reset();

  CHECK(record->GetDataState() == DataState::Intact);
  CHECK(opened.counter->ReadCount() == 0);
}

template <Cache::Strategy S>
void CheckResidentAndSparseOwnNoCluster() {
  const std::array<Alloc::Extent, 1> bad_in_the_hole{
      Alloc::Extent{.lcn = 45, .clusters = 3}};
  const std::vector<FakeDataRun> runs{FakeDataRun{.lcn = 40, .clusters = 2},
                                      FakeDataRun{.clusters = 3},
                                      FakeDataRun{.lcn = 50, .clusters = 2}};

  // Clusters 45 to 47 are bad, but the stream's hole sits at those positions
  // only: it owns no cluster, so it cannot be bad.
  CHECK(StateOfStale<S>(StaleImage(bad_in_the_hole, bad_in_the_hole, {}, runs,
                                   7 * Tests::fake_cluster_size)) ==
        DataState::Intact);

  const std::vector<BYTE> image = Support::WithBadClusters(
      Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(), {}),
      one_bad);
  const auto opened = Support::Open<S>(image);
  const auto record =
      Support::ParsedRecord(*opened.volume, Tests::mft_tree_deleted_file_idx);
  CHECK(record->GetDataState() == DataState::Intact);
}

template <Cache::Strategy S>
void CheckAMissingBadListFallsBackOnTheBitmap() {
  // Record 8 is not written: there is no bad list. The bitmap alone marks the
  // cluster allocated, so the stream is reallocated, as before this feature.
  const std::array<FakeDataRun, 1> marked{
      FakeDataRun{.lcn = 41, .clusters = 1}};
  const std::vector<BYTE> image = Support::WithBitmap(
      Tests::BuildFakeNtfsImageWithFreedNonResidentFile(stale_size, stale_runs),
      marked);

  (void)Tests::TakeCapturedLog();
  CHECK(StateOfStale<S>(image) == DataState::Reallocated);
  const std::string log = Tests::TakeCapturedLog();
  CHECK(log.find("$BadClus is not usable") != std::string::npos);
}

template <Cache::Strategy S>
void CheckACutBadListInRecoverMode() {
  // Two bad extents; the run list stops after the first.
  const std::vector<Alloc::Extent> two{{.lcn = 41, .clusters = 1},
                                       {.lcn = 43, .clusters = 1}};
  const FakeBadClusterOptions cut{.cut_run_list = true};
  const VolumeOptions recover{.include_deleted = true, .recover_errors = true};

  // Strict mode rejects the list whole: the bitmap alone answers.
  CHECK(StateOfStale<S>(StaleImage(two, two, {}, stale_runs, stale_size,
                                   cut)) == DataState::Reallocated);
  // Recover mode keeps the extent that parsed.
  CHECK(StateOfStale<S>(StaleImage(two, {}, {}, stale_runs, stale_size, cut),
                        recover) == DataState::Bad);
}

}  // namespace

TEST_CASE("a deleted stream on a bad cluster is bad, not reallocated",
          "[alloc][bad-data-state]") {
  CheckABadClusterIsBadNotReallocated<Cache::Strategy::NoCache>();
  CheckABadClusterIsBadNotReallocated<Cache::Strategy::FullCache>();
}

TEST_CASE("a reallocated cluster outranks a bad one",
          "[alloc][bad-data-state]") {
  CheckReallocatedOutranksBad<Cache::Strategy::NoCache>();
  CheckReallocatedOutranksBad<Cache::Strategy::FullCache>();
}

TEST_CASE("a bad cluster outranks an extent past the volume",
          "[alloc][bad-data-state]") {
  CheckBadOutranksAnExtentPastTheVolume<Cache::Strategy::NoCache>();
  CheckBadOutranksAnExtentPastTheVolume<Cache::Strategy::FullCache>();
}

TEST_CASE("a deleted stream with no bad cluster is intact",
          "[alloc][bad-data-state]") {
  CheckNoBadClusterIsIntact<Cache::Strategy::NoCache>();
  CheckNoBadClusterIsIntact<Cache::Strategy::FullCache>();
}

TEST_CASE("reading a bad stream is attempted and returns the same bytes",
          "[alloc][bad-data-state]") {
  CheckReadingABadStreamIsUnchanged<Cache::Strategy::NoCache>();
  CheckReadingABadStreamIsUnchanged<Cache::Strategy::FullCache>();
}

TEST_CASE("a live file is intact and nothing is read",
          "[alloc][bad-data-state]") {
  CheckALiveFileIsIntactWithoutAnyRead<Cache::Strategy::NoCache>();
  CheckALiveFileIsIntactWithoutAnyRead<Cache::Strategy::FullCache>();
}

TEST_CASE("a hole and resident data own no bad cluster",
          "[alloc][bad-data-state]") {
  CheckResidentAndSparseOwnNoCluster<Cache::Strategy::NoCache>();
  CheckResidentAndSparseOwnNoCluster<Cache::Strategy::FullCache>();
}

TEST_CASE("without a bad list the bitmap alone answers, and it is logged",
          "[alloc][bad-data-state]") {
  CheckAMissingBadListFallsBackOnTheBitmap<Cache::Strategy::NoCache>();
  CheckAMissingBadListFallsBackOnTheBitmap<Cache::Strategy::FullCache>();
}

TEST_CASE("a cut bad list is rejected when strict and kept when recovering",
          "[alloc][bad-data-state]") {
  CheckACutBadListInRecoverMode<Cache::Strategy::NoCache>();
  CheckACutBadListInRecoverMode<Cache::Strategy::FullCache>();
}
