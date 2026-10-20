#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "fake-ntfs-image.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::DataState;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeDataRun;

namespace {

// Bytes of the stale stream: four clusters at LCN 40.
constexpr ULONGLONG stale_size = 4 * Tests::fake_cluster_size;

// The freed stale file's run.
const std::vector<FakeDataRun> stale_runs{
    FakeDataRun{.lcn = 40, .clusters = 4}};

// The freed tree with a non-resident stale file, and a bitmap that marks
// allocated_runs.
std::vector<BYTE> Image(std::span<const FakeDataRun> allocated_runs) {
  return Support::WithBitmap(
      Tests::BuildFakeNtfsImageWithFreedNonResidentFile(stale_size, stale_runs),
      allocated_runs);
}

// Data states of the stale file, the freed resident file, a live file with
// resident data and a directory.
struct Picked {
  std::optional<DataState> stale;
  std::optional<DataState> freed_resident;
  std::optional<DataState> live_file;
  std::optional<DataState> directory;
  std::optional<DataState> mft;
};

Picked Pick(const Mft::MftTree& tree) {
  const auto state_of = [&tree](ULONGLONG record) {
    const Mft::MftTree::Entry* entry = tree.Find(record);
    REQUIRE(entry != nullptr);
    return entry->data_state;
  };
  return {.stale = state_of(Tests::mft_tree_stale_child_idx),
          .freed_resident = state_of(Tests::mft_tree_deleted_file_idx),
          .live_file = state_of(Tests::mft_tree_report_idx),
          .directory = state_of(Tests::mft_tree_docs_idx),
          .mft = state_of(static_cast<ULONGLONG>(Mft::Idx::Mft))};
}

template <Cache::Strategy S>
void CheckNothingIsAnnotatedWithoutIncludeDeleted() {
  const std::vector<BYTE> image = Image({});
  const auto opened =
      Support::Open<S>(image, VolumeOptions{.include_deleted = false});

  const Mft::MftTree tree(*opened.volume);

  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    CHECK_FALSE(entry.data_state);
  }
  CHECK(tree.Stats().data_intact == 0);
  CHECK(tree.Stats().data_reallocated == 0);
  CHECK(tree.Stats().data_unknown == 0);
  CHECK(opened.counter->ReadsInRange(Support::bitmap_data_offset,
                                     image.size()) == 0);
}

template <Cache::Strategy S>
void CheckDeletedEntriesGetTheirState(bool reused) {
  const std::array<FakeDataRun, 1> reused_cluster{
      FakeDataRun{.lcn = 42, .clusters = 1}};
  const auto opened = Support::Open<S>(
      Image(reused ? std::span<const FakeDataRun>(reused_cluster)
                   : std::span<const FakeDataRun>{}));

  const Mft::MftTree tree(*opened.volume);
  const Picked picked = Pick(tree);

  CHECK(picked.stale == (reused ? DataState::Reallocated : DataState::Intact));
  CHECK(picked.live_file == DataState::Intact);
  CHECK(picked.mft == DataState::Intact);
  CHECK_FALSE(picked.directory);
}

template <Cache::Strategy S>
void CheckTheAnnotationCanBeTurnedOff() {
  const std::vector<BYTE> image = Image({});
  const auto opened = Support::Open<S>(image);

  const Mft::MftTree tree(*opened.volume, {.annotate_data_state = false});

  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    CHECK_FALSE(entry.data_state);
  }
  CHECK(opened.counter->ReadsInRange(Support::bitmap_data_offset,
                                     image.size()) == 0);
}

template <Cache::Strategy S>
void CheckTheCountersAddUp() {
  const std::array<FakeDataRun, 1> reused_cluster{
      FakeDataRun{.lcn = 41, .clusters = 1}};
  const auto opened = Support::Open<S>(Image(reused_cluster));
  (void)Tests::TakeCapturedLog();

  const Mft::MftTree tree(*opened.volume);

  ULONGLONG intact = 0;
  ULONGLONG reallocated = 0;
  ULONGLONG unknown = 0;
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    if (!entry.data_state) {
      continue;
    }
    intact += *entry.data_state == DataState::Intact ? 1 : 0;
    reallocated += *entry.data_state == DataState::Reallocated ? 1 : 0;
    unknown += *entry.data_state == DataState::Unknown ? 1 : 0;
  }
  const Mft::MftTree::ScanStats& stats = tree.Stats();

  CHECK(reallocated == 1);
  CHECK(stats.data_intact == intact);
  CHECK(stats.data_reallocated == reallocated);
  CHECK(stats.data_unknown == unknown);
  CHECK_THAT(Tests::TakeCapturedLog(),
             ContainsSubstring("MFT scan data: " + std::to_string(intact) +
                               " intact, 1 reallocated, 0 unknown"));
}

template <Cache::Strategy S>
void CheckAMissingBitmapDoesNotStopTheScan() {
  // No bitmap: record 6 is zero-filled.
  const auto opened =
      Support::Open<S>(Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
          stale_size, stale_runs));
  const auto without = Support::Open<S>(
      Tests::BuildFakeNtfsImageWithFreedNonResidentFile(stale_size, stale_runs),
      VolumeOptions{.include_deleted = true});

  const Mft::MftTree tree(*opened.volume, {.annotate_data_state = true});
  const Mft::MftTree plain(*without.volume, {.annotate_data_state = false});

  CHECK(tree.Entries().size() == plain.Entries().size());
  const Picked picked = Pick(tree);
  CHECK(picked.stale == DataState::Unknown);
  CHECK(picked.live_file == DataState::Intact);
  CHECK(tree.Stats().data_unknown >= 1);
}

}  // namespace

TEST_CASE(
    "a scan without include_deleted annotates nothing and reads no bitmap",
    "[mft][data-state]") {
  CheckNothingIsAnnotatedWithoutIncludeDeleted<Cache::Strategy::NoCache>();
  CheckNothingIsAnnotatedWithoutIncludeDeleted<Cache::Strategy::FullCache>();
}

TEST_CASE("a deleted stream on free clusters is intact in a scan",
          "[mft][data-state]") {
  CheckDeletedEntriesGetTheirState<Cache::Strategy::NoCache>(false);
  CheckDeletedEntriesGetTheirState<Cache::Strategy::FullCache>(false);
}

TEST_CASE("a deleted stream on a reused cluster is reallocated in a scan",
          "[mft][data-state]") {
  CheckDeletedEntriesGetTheirState<Cache::Strategy::NoCache>(true);
  CheckDeletedEntriesGetTheirState<Cache::Strategy::FullCache>(true);
}

TEST_CASE("the annotation can be turned off", "[mft][data-state]") {
  CheckTheAnnotationCanBeTurnedOff<Cache::Strategy::NoCache>();
  CheckTheAnnotationCanBeTurnedOff<Cache::Strategy::FullCache>();
}

TEST_CASE("the scan counters add up and reach the log", "[mft][data-state]") {
  CheckTheCountersAddUp<Cache::Strategy::NoCache>();
  CheckTheCountersAddUp<Cache::Strategy::FullCache>();
}

TEST_CASE("a bitmap that cannot be read leaves the scan whole",
          "[mft][data-state]") {
  CheckAMissingBitmapDoesNotStopTheScan<Cache::Strategy::NoCache>();
  CheckAMissingBitmapDoesNotStopTheScan<Cache::Strategy::FullCache>();
}
