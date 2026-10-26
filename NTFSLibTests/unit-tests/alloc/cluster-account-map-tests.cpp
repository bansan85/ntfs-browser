#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <optional>
#include <string>
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
using Alloc::ClusterPiece;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::Freed;
using Support::Live;
using Support::Stream;
using Tests::FakeAccountFile;
using Tests::FakeAccountStream;
using Tests::FakeDataRun;

namespace {

constexpr ULONGLONG subject = 22;
constexpr ULONGLONG live_owner = 17;
constexpr ULONGLONG cluster = Tests::fake_cluster_size;

template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
TreeSupport::ScannedVolume<S>
    ScanMap(const Support::Volume& volume,
            const VolumeOptions& options = AllocSupport::DeletedOptions()) {
  return TreeSupport::ScannedVolume<S>(
      volume.Build(), options,
      Support::Scan(Alloc::ClusterAccountLevel::CountsAndMap));
}

// The pieces of the one stream of record's map.
const std::vector<ClusterPiece>&
    PiecesOf(const NTFS_BROWSER_NAMESPACE::Mft::MftTree& tree, ULONGLONG record,
             size_t stream = 0) {
  const Alloc::ClusterAccount& account = Support::AccountOf(tree, record);
  REQUIRE(account.streams.size() > stream);
  return account.streams.at(stream).pieces;
}

// Checks the pieces are in VCN order, touch, and cover [0, clusters).
void CheckCovers(const std::vector<ClusterPiece>& pieces, ULONGLONG clusters) {
  ULONGLONG next = 0;
  for (const ClusterPiece& piece : pieces) {
    CHECK(piece.first_vcn == next);
    CHECK(piece.clusters != 0);
    next = piece.first_vcn + piece.clusters;
  }
  CHECK(next == clusters);
}

void CheckPiece(const ClusterPiece& piece, ULONGLONG vcn, ULONGLONG clusters,
                std::optional<ULONGLONG> lcn, ClusterCondition condition) {
  CHECK(piece.first_vcn == vcn);
  CHECK(piece.clusters == clusters);
  CHECK(piece.lcn == lcn);
  CHECK(piece.condition == condition);
}

}  // namespace

TEST_CASE("the map is in file order when the runs are not in LCN order",
          "[alloc][cluster][map]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    // VCN 0 and 1 at a high LCN, VCN 2 and 3 at a low one.
    volume.Put(subject, Freed(100, Stream({{50, 2}, {40, 2}}, 4 * cluster)));
    // A live file rewrote the last cluster of the stream, at LCN 41.
    volume.Put(live_owner, Live(Stream({{41, 1}}, cluster)));
    volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 1}};
    const auto scanned = ScanMap<decltype(strategy)::value>(volume);

    const std::vector<ClusterPiece>& pieces = PiecesOf(scanned.tree, subject);
    // The first two pieces touch in the file and share a condition: one piece.
    REQUIRE(pieces.size() == 2);
    CheckPiece(pieces.at(0), 0, 3, 50, ClusterCondition::Intact);
    CheckPiece(pieces.at(1), 3, 1, 41, ClusterCondition::ReallocatedWritten);
    CheckCovers(pieces, 4);
  });
}

TEST_CASE("the map keeps sparse holes and the clusters a cut list lost",
          "[alloc][cluster][map]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    FakeAccountFile file =
        Freed(100, Stream({{40, 2},
                           FakeDataRun{.lcn = std::nullopt, .clusters = 3},
                           {50, 1},
                           {54, 2}},
                          8 * cluster));
    file.streams.front().cut_after_runs = 3;
    volume.Put(subject, file);
    const auto scanned = ScanMap<decltype(strategy)::value>(
        volume, Support::DeletedAndRecover());

    const std::vector<ClusterPiece>& pieces = PiecesOf(scanned.tree, subject);
    REQUIRE(pieces.size() == 4);
    CheckPiece(pieces.at(0), 0, 2, 40, ClusterCondition::Intact);
    CheckPiece(pieces.at(1), 2, 3, std::nullopt, ClusterCondition::Sparse);
    CheckPiece(pieces.at(2), 5, 1, 50, ClusterCondition::Intact);
    CheckPiece(pieces.at(3), 6, 2, std::nullopt, ClusterCondition::Invalid);
    CheckCovers(pieces, 8);
  });
}

TEST_CASE("the map of an entry has one stream for each $DATA, named ones too",
          "[alloc][cluster][map]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    FakeAccountFile file = Freed(100, Stream({{40, 2}}, 2 * cluster));
    Tests::FakeAccountStream named = Stream({{50, 1}}, cluster);
    named.name = L"zone";
    file.streams.push_back(named);
    volume.Put(subject, file);
    const auto scanned = ScanMap<decltype(strategy)::value>(volume);

    const Alloc::ClusterAccount& account =
        Support::AccountOf(scanned.tree, subject);
    REQUIRE(account.streams.size() == 2);
    CHECK(account.streams.at(0).name.empty());
    CHECK(account.streams.at(1).name == L"zone");
    CHECK(account.streams.at(0).data_size == 2 * cluster);
    CHECK(account.tally.Count(ClusterCondition::Intact) == 3);
    CheckCovers(account.streams.at(1).pieces, 1);
  });
}

TEST_CASE("the counts level keeps no map", "[alloc][cluster][map]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    volume.Put(subject, Freed(100, Stream({{40, 2}}, 2 * cluster)));
    const TreeSupport::ScannedVolume<decltype(strategy)::value> scanned(
        volume.Build(), AllocSupport::DeletedOptions(), Support::Scan());

    CHECK(Support::AccountOf(scanned.tree, subject).streams.empty());
  });
}

TEST_CASE("a damaged compression unit marks all of its clusters",
          "[alloc][cluster][map]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    // Two units of four clusters, each with two real clusters and two holes.
    FakeAccountStream stream =
        Stream({{40, 2},
                FakeDataRun{.lcn = std::nullopt, .clusters = 2},
                {44, 2},
                FakeDataRun{.lcn = std::nullopt, .clusters = 2}},
               8 * cluster);
    stream.comp_unit_size = Tests::compression_unit_size_shift;
    volume.Put(subject, Freed(100, stream));
    // A live file rewrote the second real cluster of the first unit.
    volume.Put(live_owner, Live(Stream({{41, 1}}, cluster)));
    volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 1}};
    const auto scanned = ScanMap<decltype(strategy)::value>(volume);

    const Alloc::ClusterAccount& account =
        Support::AccountOf(scanned.tree, subject);
    CHECK(account.tally.Count(ClusterCondition::LostWithUnit) == 1);
    CHECK(account.tally.Count(ClusterCondition::ReallocatedWritten) == 1);
    CHECK(account.tally.Count(ClusterCondition::Intact) == 2);
    CHECK(account.tally.Count(ClusterCondition::Sparse) == 4);

    const std::vector<ClusterPiece>& pieces = account.streams.at(0).pieces;
    REQUIRE(pieces.size() == 5);
    CheckPiece(pieces.at(0), 0, 1, 40, ClusterCondition::LostWithUnit);
    CheckPiece(pieces.at(1), 1, 1, 41, ClusterCondition::ReallocatedWritten);
    CheckPiece(pieces.at(2), 2, 2, std::nullopt,
               ClusterCondition::LostWithUnit);
    CheckPiece(pieces.at(3), 4, 2, 44, ClusterCondition::Intact);
    CheckPiece(pieces.at(4), 6, 2, std::nullopt, ClusterCondition::Sparse);
    CheckCovers(pieces, 8);
  });
}

TEST_CASE("a map turns into byte ranges, clipped at the file size",
          "[alloc][cluster][map]") {
  Alloc::StreamClusterMap map;
  map.data_size = 5 * cluster + cluster / 2;
  map.initialized_size = 3 * cluster;
  map.pieces = {{.first_vcn = 0,
                 .clusters = 2,
                 .lcn = 40,
                 .condition = ClusterCondition::Intact},
                {.first_vcn = 2,
                 .clusters = 1,
                 .lcn = 42,
                 .condition = ClusterCondition::ReallocatedWritten},
                {.first_vcn = 3,
                 .clusters = 2,
                 .lcn = std::nullopt,
                 .condition = ClusterCondition::Sparse},
                {.first_vcn = 5,
                 .clusters = 2,
                 .lcn = 50,
                 .condition = ClusterCondition::NeverWritten},
                {.first_vcn = 7,
                 .clusters = 1,
                 .lcn = 60,
                 .condition = ClusterCondition::Bad}};

  const std::vector<Alloc::StreamByteRange> ranges =
      Alloc::ToByteRanges(map, cluster);

  // The piece at VCN 7 starts past the end of the file: it is left out.
  REQUIRE(ranges.size() == 4);
  CHECK(ranges.at(0).offset == 0);
  CHECK(ranges.at(0).length == 2 * cluster);
  CHECK_FALSE(ranges.at(0).damaged);
  CHECK_FALSE(ranges.at(0).reads_as_zeros);
  CHECK(ranges.at(1).offset == 2 * cluster);
  CHECK(ranges.at(1).damaged);
  CHECK(ranges.at(2).condition == ClusterCondition::Sparse);
  CHECK(ranges.at(2).reads_as_zeros);
  CHECK_FALSE(ranges.at(2).damaged);
  // Clipped at the file size, not at the end of its last cluster.
  CHECK(ranges.at(3).offset == 5 * cluster);
  CHECK(ranges.at(3).length == cluster / 2);
  CHECK(ranges.at(3).condition == ClusterCondition::NeverWritten);
  CHECK(ranges.at(3).reads_as_zeros);
  CHECK_FALSE(ranges.at(3).damaged);
}

TEST_CASE("a never written range of a scanned file reads as zeros, not damage",
          "[alloc][cluster][map]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume;
    FakeAccountFile file = Freed(100, Stream({{40, 4}}, 4 * cluster));
    file.streams.front().ini_size = cluster;
    volume.Put(subject, file);
    const auto scanned = ScanMap<decltype(strategy)::value>(volume);

    const std::vector<Alloc::StreamByteRange> ranges = Alloc::ToByteRanges(
        Support::AccountOf(scanned.tree, subject).streams.at(0), cluster);

    REQUIRE(ranges.size() == 2);
    CHECK(ranges.at(0).condition == ClusterCondition::Intact);
    CHECK(ranges.at(1).offset == cluster);
    CHECK(ranges.at(1).length == 3 * cluster);
    CHECK(ranges.at(1).reads_as_zeros);
    CHECK_FALSE(ranges.at(1).damaged);
  });
}

TEST_CASE("every condition has its own name and a verdict",
          "[alloc][cluster][map]") {
  std::vector<std::string> names;
  for (size_t i = 0; i < Alloc::cluster_condition_count; i++) {
    const auto condition = static_cast<ClusterCondition>(i);
    const std::string name = Alloc::ConditionName(condition);
    CHECK_FALSE(name.empty());
    CHECK(std::ranges::find(names, name) == names.end());
    names.push_back(name);

    const bool fine = condition == ClusterCondition::Intact ||
                      condition == ClusterCondition::IntactChecked ||
                      condition == ClusterCondition::Consistent ||
                      condition == ClusterCondition::Sparse ||
                      condition == ClusterCondition::NeverWritten;
    INFO(name);
    CHECK(Alloc::IsDamaged(condition) == !fine);
  }
  CHECK(Alloc::ClusterTally{}.Empty());
  Alloc::ClusterTally tally;
  tally.Add(ClusterCondition::Bad, 3);
  Alloc::ClusterTally other;
  other.Add(ClusterCondition::Bad, 4);
  other.Add(ClusterCondition::Sparse, 1);
  tally.Add(other);
  CHECK_FALSE(tally.Empty());
  CHECK(tally.Count(ClusterCondition::Bad) == 7);
  CHECK(tally.Count(ClusterCondition::Sparse) == 1);
}
