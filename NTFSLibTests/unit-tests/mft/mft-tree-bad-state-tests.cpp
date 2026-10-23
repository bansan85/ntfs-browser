#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-failure-support.h"
#include "alloc-test-support.h"
#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "mft-tree-state-support.h"
#include "mft/tree-testing.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
namespace TreeSupport = NTFS_BROWSER_NAMESPACE::Tests::MftTreeSupport;
using Alloc::DataState;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::stale_size;
using Tests::FakeDataRun;
using TreeSupport::CheckNoEntryIsAnnotated;
using TreeSupport::RequiredEntry;
using TreeSupport::ScannedVolume;

namespace {

// The freed stale file's run.
const std::vector<FakeDataRun> stale_runs{
    FakeDataRun{.lcn = 40, .clusters = 4}};

// One bad cluster inside the stale file's run, marked allocated in the bitmap.
const std::vector<Alloc::Extent> one_bad{{.lcn = 41, .clusters = 1}};

// The freed tree with a non-resident stale file, a bitmap that marks the bad
// cluster, and, when with_list is set, the bad list.
std::vector<BYTE> Image(bool with_list = true) {
  std::vector<BYTE> image = Support::WithBitmap(
      Tests::BuildFakeNtfsImageWithFreedNonResidentFile(stale_size, stale_runs),
      Support::WithBadRuns({}, one_bad));
  return with_list ? Support::WithBadClusters(std::move(image), one_bad)
                   : image;
}

// The same volume with a $BadClus that holds no $Bad stream: the list is
// unusable, and the library says so once, as soon as it tries to load it. The
// scan reads record 8 itself like any other record, so its reads cannot tell
// whether the list was loaded. That message can.
std::vector<BYTE> UnusableListImage() {
  return Support::WithBadClusters(
      Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
                              stale_size, stale_runs),
                          Support::WithBadRuns({}, one_bad)),
      one_bad, {.omit_bad_stream = true});
}

// The message of a bad list that could not be loaded.
constexpr std::string_view unusable_message = "$BadClus is not usable";

template <Cache::Strategy S>
void CheckADeletedEntryOnABadClusterIsBad() {
  const ScannedVolume<S> scanned(Image());

  CHECK(
      RequiredEntry(scanned.tree, Tests::mft_tree_stale_child_idx).data_state ==
      DataState::Bad);
  CHECK(RequiredEntry(scanned.tree, Tests::mft_tree_report_idx).data_state ==
        DataState::Intact);
}

template <Cache::Strategy S>
void CheckTheFourCountersAddUpAndReachTheLog() {
  (void)Tests::TakeCapturedLog();
  const ScannedVolume<S> scanned(Image());
  const Mft::MftTree& tree = scanned.tree;

  ULONGLONG annotated = 0;
  ULONGLONG bad = 0;
  for (const Mft::MftTree::Entry& entry : tree.Entries()) {
    if (entry.data_state) {
      annotated++;
      bad += *entry.data_state == DataState::Bad ? 1 : 0;
    }
  }
  const Mft::MftTree::ScanStats& stats = tree.Stats();

  CHECK(bad == 1);
  CHECK(stats.data_bad == bad);
  CHECK(stats.data_intact + stats.data_reallocated + stats.data_unknown +
            stats.data_bad ==
        annotated);
  CHECK_THAT(Tests::TakeCapturedLog(),
             ContainsSubstring(" reallocated, " +
                               std::to_string(stats.data_unknown) +
                               " unknown, 1 bad"));
}

template <Cache::Strategy S>
void CheckNoBadListIsLoadedWhenNothingIsAnnotated() {
  const std::vector<BYTE> image = UnusableListImage();

  for (const bool include_deleted : {false, true}) {
    const Support::CapturedLevels log;
    const ScannedVolume<S> scanned(
        image, VolumeOptions{.include_deleted = include_deleted},
        {.annotate_data_state = false});

    CheckNoEntryIsAnnotated(scanned.tree);
    CHECK(scanned.tree.Stats().data_bad == 0);
    CHECK(scanned.opened.counter->ReadsInRange(Support::bitmap_data_offset,
                                               image.size()) == 0);
    CHECK(log.Count(unusable_message) == 0);
  }
}

template <Cache::Strategy S>
void CheckWithoutIncludeDeletedNoBadListIsLoaded() {
  const Support::CapturedLevels log;
  const ScannedVolume<S> scanned(UnusableListImage(),
                                 VolumeOptions{.include_deleted = false});

  CheckNoEntryIsAnnotated(scanned.tree);
  CHECK(log.Count(unusable_message) == 0);
}

template <Cache::Strategy S>
void CheckAnAnnotatedScanLoadsTheBadListOnce() {
  const Support::CapturedLevels log;
  const ScannedVolume<S> scanned(UnusableListImage());

  // The bitmap alone answers: the bad cluster is marked allocated in it.
  CHECK(
      RequiredEntry(scanned.tree, Tests::mft_tree_stale_child_idx).data_state ==
      DataState::Reallocated);
  CHECK(log.Count(unusable_message) == 1);
}

template <Cache::Strategy S>
void CheckAMissingBadListLeavesTheBitmapAlone() {
  // Record 8 is zero-filled: the bitmap alone marks the cluster allocated.
  const ScannedVolume<S> scanned(Image(false));

  CHECK(
      RequiredEntry(scanned.tree, Tests::mft_tree_stale_child_idx).data_state ==
      DataState::Reallocated);
  CHECK(scanned.tree.Stats().data_bad == 0);
}

template <Cache::Strategy S>
void CheckSeveralFlushesGiveTheSameStates() {
  const Tests::FakeManyDeleted built =
      Tests::BuildFakeNtfsImageWithManyDeletedFiles(8, 2);
  // Every other file's first cluster is bad and marked in the bitmap.
  std::vector<Alloc::Extent> bad;
  std::vector<FakeDataRun> marked;
  for (size_t i = 0; i < built.lcns.size(); i += 2) {
    bad.push_back({.lcn = built.lcns[i], .clusters = 1});
  }
  std::vector<BYTE> image =
      Support::WithBitmap(built.image, Support::WithBadRuns(marked, bad));
  image = Support::WithBadClusters(std::move(image), bad);

  const auto states_with_cap = [&](size_t cap) {
    Mft::SetMaxPendingRangesForTests(cap);
    const auto opened = Support::Open<S>(image);
    const Mft::MftTree tree(*opened.volume);
    Mft::SetMaxPendingRangesForTests(0);
    std::vector<std::optional<DataState>> states;
    for (size_t i = 0; i < built.lcns.size(); i++) {
      const Mft::MftTree::Entry* entry =
          tree.Find(Tests::many_deleted_first_idx + i);
      REQUIRE(entry != nullptr);
      states.push_back(entry->data_state);
    }
    return states;
  };

  const auto whole = states_with_cap(0);
  const auto flushed = states_with_cap(2);

  CHECK(whole == flushed);
  for (size_t i = 0; i < whole.size(); i++) {
    CHECK(whole[i] == (i % 2 == 0 ? DataState::Bad : DataState::Intact));
  }
}

}  // namespace

TEST_CASE("a deleted entry on a bad cluster is bad in a scan",
          "[mft][bad-state]") {
  CheckADeletedEntryOnABadClusterIsBad<Cache::Strategy::NoCache>();
  CheckADeletedEntryOnABadClusterIsBad<Cache::Strategy::FullCache>();
}

TEST_CASE("the four scan counters add up and reach the log",
          "[mft][bad-state]") {
  CheckTheFourCountersAddUpAndReachTheLog<Cache::Strategy::NoCache>();
  CheckTheFourCountersAddUpAndReachTheLog<Cache::Strategy::FullCache>();
}

TEST_CASE("a scan that does not annotate never loads the bad list",
          "[mft][bad-state]") {
  CheckNoBadListIsLoadedWhenNothingIsAnnotated<Cache::Strategy::NoCache>();
  CheckNoBadListIsLoadedWhenNothingIsAnnotated<Cache::Strategy::FullCache>();
}

TEST_CASE("a scan without include_deleted never loads the bad list",
          "[mft][bad-state]") {
  CheckWithoutIncludeDeletedNoBadListIsLoaded<Cache::Strategy::NoCache>();
  CheckWithoutIncludeDeletedNoBadListIsLoaded<Cache::Strategy::FullCache>();
}

TEST_CASE("an annotated scan loads the bad list once", "[mft][bad-state]") {
  CheckAnAnnotatedScanLoadsTheBadListOnce<Cache::Strategy::NoCache>();
  CheckAnAnnotatedScanLoadsTheBadListOnce<Cache::Strategy::FullCache>();
}

TEST_CASE("a scan without a bad list leaves the bitmap to answer",
          "[mft][bad-state]") {
  CheckAMissingBadListLeavesTheBitmapAlone<Cache::Strategy::NoCache>();
  CheckAMissingBadListLeavesTheBitmapAlone<Cache::Strategy::FullCache>();
}

TEST_CASE("several flushes give the states of one", "[mft][bad-state]") {
  CheckSeveralFlushesGiveTheSameStates<Cache::Strategy::NoCache>();
  CheckSeveralFlushesGiveTheSameStates<Cache::Strategy::FullCache>();
}
