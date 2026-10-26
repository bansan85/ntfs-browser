#include <ntfs-browser/win-types.h>

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "cluster-account-support.h"
#include "fake-ntfs-image.h"
#include "mft-tree-state-support.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace AllocSupport = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AccountSupport;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace TreeSupport = NTFS_BROWSER_NAMESPACE::Tests::MftTreeSupport;
using Alloc::ClusterCondition;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::CountOf;
using Support::Freed;
using Support::Live;
using Support::Stream;
using Tests::FakeAccountFile;
using Tests::FakeAccountStream;
using Tests::FakeDataRun;

namespace {

// The freed file all the tests judge, and the one that disturbs it.
constexpr ULONGLONG subject = 22;
constexpr ULONGLONG other = 23;
constexpr ULONGLONG live_owner = 17;

// The subject's clusters: four, at 40 to 43.
const std::vector<FakeDataRun> subject_runs{
    FakeDataRun{.lcn = 40, .clusters = 4}};

// Cluster size of the fake volumes, and four clusters of bytes.
constexpr ULONGLONG cluster = Tests::fake_cluster_size;
constexpr ULONGLONG four_clusters = 4 * cluster;

template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
TreeSupport::ScannedVolume<S>
    ScanVolume(const Support::Volume& volume,
               const VolumeOptions& options = AllocSupport::DeletedOptions()) {
  return TreeSupport::ScannedVolume<S>(volume.Build(), options,
                                       Support::Scan());
}

// Counts of the subject's tally, under the conditions the check lists, and
// nothing under the others.
void CheckOnly(
    const Alloc::ClusterAccount& account,
    std::initializer_list<std::pair<ClusterCondition, ULONGLONG>> expected) {
  Alloc::ClusterTally tally;
  for (const auto& [condition, clusters] : expected) {
    tally.Add(condition, clusters);
  }
  for (size_t i = 0; i < Alloc::cluster_condition_count; i++) {
    const auto condition = static_cast<ClusterCondition>(i);
    INFO(Alloc::ConditionName(condition));
    CHECK(account.tally.Count(condition) == tally.Count(condition));
  }
}

}  // namespace

TEST_CASE("a freed file nobody else claims is intact", "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(subject, Freed(100, Stream(subject_runs, four_clusters)));
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Intact, 4}});
    CHECK(scanned.tree.Stats().cluster_accounts == 1);
    CHECK(scanned.tree.Stats().clusters.Count(ClusterCondition::Intact) == 4);
  });
}

TEST_CASE("a freed file's clusters that $Bitmap marks allocated, with no owner",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(subject, Freed(100, Stream(subject_runs, four_clusters)));
    volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 2}};
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Intact, 2},
               {ClusterCondition::AllocatedUnknownOwner, 2}});
  });
}

TEST_CASE("a live file over a freed file's clusters rewrites them",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(subject, Freed(100, Stream(subject_runs, four_clusters)));
    // The live file wrote cluster 41, and has not written cluster 42 yet.
    FakeAccountFile owner = Live(Stream({{41, 2}}, 2 * cluster));
    owner.streams.front().ini_size = cluster;
    volume.Put(live_owner, owner);
    volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 2}};
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Intact, 2},
               {ClusterCondition::ReallocatedWritten, 1},
               {ClusterCondition::ReallocatedUnwritten, 1}});
  });
}

TEST_CASE("the later freed of two claimants keeps the clusters",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(subject, Freed(100, Stream(subject_runs, four_clusters)));
    volume.Put(other, Freed(200, Stream({{41, 2}}, 2 * cluster)));
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Intact, 2},
               {ClusterCondition::OverwrittenThenFreed, 2}});
    CheckOnly(Support::AccountOf(scanned.tree, other),
              {{ClusterCondition::Intact, 2}});
  });
}

TEST_CASE("two freed claimants that no sequence number orders are contested",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    for (const ULONGLONG other_lsn : {ULONGLONG{100}, ULONGLONG{0}}) {
      Support::Volume volume;
      volume.Put(subject, Freed(100, Stream(subject_runs, four_clusters)));
      volume.Put(other, Freed(other_lsn, Stream({{41, 2}}, 2 * cluster)));
      const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

      INFO("lsn of the other: " << other_lsn);
      CheckOnly(
          Support::AccountOf(scanned.tree, subject),
          {{ClusterCondition::Intact, 2}, {ClusterCondition::Contested, 2}});
      CheckOnly(Support::AccountOf(scanned.tree, other),
                {{ClusterCondition::Contested, 2}});
    }
  });
}

TEST_CASE("a freed claimant that never wrote a cluster does not hurt it",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(subject, Freed(100, Stream(subject_runs, four_clusters)));
    FakeAccountFile later = Freed(200, Stream({{41, 2}}, 2 * cluster));
    later.streams.front().ini_size = 0;
    volume.Put(other, later);
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Intact, 4}});
    CheckOnly(Support::AccountOf(scanned.tree, other),
              {{ClusterCondition::NeverWritten, 2}});
  });
}

TEST_CASE("the clusters past a file's own valid data length were never written",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    FakeAccountFile file = Freed(100, Stream(subject_runs, four_clusters));
    // Two clusters and a half were written.
    file.streams.front().ini_size = 2 * cluster + cluster / 2;
    volume.Put(subject, file);
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(
        Support::AccountOf(scanned.tree, subject),
        {{ClusterCondition::Intact, 3}, {ClusterCondition::NeverWritten, 1}});
  });
}

TEST_CASE("a bad cluster, and a run past the volume, are told apart",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(subject, Freed(100, Stream({{40, 4}, {Support::far_lcn, 2}},
                                          6 * cluster)));
    volume.bad = {Alloc::Extent{.lcn = 42, .clusters = 1}};
    volume.allocated =
        Support::Volume{}.allocated;  // nothing else is allocated
    volume.allocated.push_back(FakeDataRun{.lcn = 42, .clusters = 1});
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Intact, 3},
               {ClusterCondition::Bad, 1},
               {ClusterCondition::Invalid, 2}});
  });
}

TEST_CASE("without $Bitmap the allocation of every cluster is unknown",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.with_bitmap = false;
    volume.Put(subject, Freed(100, Stream(subject_runs, four_clusters)));
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Unknown, 4}});
  });
}

TEST_CASE("sparse holes are counted apart, and a cut run list is invalid",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(
        subject,
        Freed(100, Stream({{40, 2},
                           FakeDataRun{.lcn = std::nullopt, .clusters = 3},
                           {44, 1}},
                          6 * cluster)));
    // The list ends after the hole: the last run is lost.
    FakeAccountFile cut = Freed(100, Stream({{50, 2}, {54, 2}}, 4 * cluster));
    cut.streams.front().cut_after_runs = 1;
    volume.Put(other, cut);
    const auto scanned = ScanVolume<decltype(strategy)::value>(
        volume, Support::DeletedAndRecover());

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Intact, 3}, {ClusterCondition::Sparse, 3}});
    CheckOnly(Support::AccountOf(scanned.tree, other),
              {{ClusterCondition::Intact, 2}, {ClusterCondition::Invalid, 2}});
  });
}

TEST_CASE("a damaged live file is consistent, or free but claimed",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    FakeAccountFile damaged = Live(Stream({{40, 2}, {50, 2}}, 4 * cluster));
    damaged.streams.front().cut_after_runs = 2;
    volume.Put(live_owner, damaged);
    // Only the first run is allocated.
    volume.allocated = {FakeDataRun{.lcn = 40, .clusters = 2}};
    const auto scanned = ScanVolume<decltype(strategy)::value>(
        volume, Support::DeletedAndRecover());

    CHECK(scanned.tree.Stats().damaged >= 1);
    CheckOnly(Support::AccountOf(scanned.tree, live_owner),
              {{ClusterCondition::Consistent, 2},
               {ClusterCondition::FreeButClaimed, 2}});
  });
}

TEST_CASE("a damaged live file over another's clusters is cross-linked",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    FakeAccountFile damaged = Live(Stream({{40, 4}}, 4 * cluster));
    damaged.streams.front().cut_after_runs = 1;
    volume.Put(live_owner, damaged);
    volume.Put(18, Live(Stream({{42, 4}}, 4 * cluster)));
    volume.allocated = {FakeDataRun{.lcn = 40, .clusters = 6}};
    const auto scanned = ScanVolume<decltype(strategy)::value>(
        volume, Support::DeletedAndRecover());

    CheckOnly(Support::AccountOf(scanned.tree, live_owner),
              {{ClusterCondition::Consistent, 2},
               {ClusterCondition::CrossLinked, 2}});
    // The other live file is not damaged: it has no account.
    CHECK(scanned.tree.ClusterAccountOf(18) == nullptr);
  });
}

TEST_CASE("a damaged live file on bad clusters, past the volume or unknown",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    FakeAccountFile damaged =
        Live(Stream({{40, 2}, {Support::far_lcn, 2}}, 4 * cluster));
    damaged.streams.front().cut_after_runs = 2;
    volume.Put(live_owner, damaged);
    volume.bad = {Alloc::Extent{.lcn = 41, .clusters = 1}};
    volume.allocated = {FakeDataRun{.lcn = 40, .clusters = 2}};
    const auto scanned = ScanVolume<decltype(strategy)::value>(
        volume, Support::DeletedAndRecover());

    CheckOnly(Support::AccountOf(scanned.tree, live_owner),
              {{ClusterCondition::Consistent, 1},
               {ClusterCondition::Bad, 1},
               {ClusterCondition::Invalid, 2}});

    Support::Volume blind;
    blind.with_bitmap = false;
    blind.Put(live_owner, damaged);
    const auto unknown = ScanVolume<decltype(strategy)::value>(
        blind, Support::DeletedAndRecover());
    CHECK(CountOf(unknown.tree, live_owner, ClusterCondition::Unknown) == 4);
  });
}

TEST_CASE("a live file that is not damaged has no account",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(live_owner, Live(Stream({{40, 2}}, 2 * cluster)));
    volume.allocated = {FakeDataRun{.lcn = 40, .clusters = 2}};
    const auto scanned = ScanVolume<decltype(strategy)::value>(
        volume, Support::DeletedAndRecover());

    CHECK(scanned.tree.ClusterAccountOf(live_owner) == nullptr);
  });
}

TEST_CASE(
    "the account is off by default, and for a volume with nothing to keep",
    "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    constexpr auto S = decltype(strategy)::value;
    Support::Volume volume;
    volume.Put(subject, Freed(100, Stream(subject_runs, four_clusters)));

    const TreeSupport::ScannedVolume<S> off(volume.Build());
    CHECK(off.tree.ClusterAccountOf(subject) == nullptr);
    CHECK(off.tree.Stats().cluster_accounts == 0);

    const TreeSupport::ScannedVolume<S> nothing_to_keep(
        volume.Build(), VolumeOptions{}, Support::Scan());
    CHECK(nothing_to_keep.tree.Stats().cluster_accounts == 0);
  });
}

TEST_CASE("an encrypted stream gets the same counts", "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    FakeAccountStream encrypted = Stream(subject_runs, four_clusters);
    encrypted.encrypted = true;
    volume.Put(subject, Freed(100, encrypted));
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Intact, 4}});
  });
}

TEST_CASE("the clusters of every non-resident attribute are claimed",
          "[alloc][cluster]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(subject, Freed(100, Stream(subject_runs, four_clusters)));
    // A live file whose $INDEX_ALLOCATION sits on cluster 41, and a $BITMAP of
    // another on 42.
    FakeAccountFile owner = Live(Stream({}, 0));
    owner.streams.clear();
    FakeAccountStream index;
    index.type = NTFS_BROWSER_NAMESPACE::Attr::Type::IndexAllocation;
    index.runs = {FakeDataRun{.lcn = 41, .clusters = 1}};
    index.real_size = cluster;
    owner.streams.push_back(index);
    FakeAccountStream bitmap = index;
    bitmap.type = NTFS_BROWSER_NAMESPACE::Attr::Type::Bitmap;
    bitmap.runs = {FakeDataRun{.lcn = 42, .clusters = 1}};
    owner.streams.push_back(bitmap);
    volume.Put(live_owner, owner);
    volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 2}};
    const auto scanned = ScanVolume<decltype(strategy)::value>(volume);

    CheckOnly(Support::AccountOf(scanned.tree, subject),
              {{ClusterCondition::Intact, 2},
               {ClusterCondition::ReallocatedWritten, 2}});
  });
}
