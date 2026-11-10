#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/alloc/cluster-account-support.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/mft/mft-tree-state-support.h>
#include <ntfs-browser/test/support/scripted-disk-reader.h>

#include "alloc/cluster-account.h"
#include "cache/strategy.h"
#include "mft/tree.h"
#include "ntfs-volume.h"
#include "win-types.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace TestAlloc = NTFS_BROWSER_NAMESPACE::Test::Alloc;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace TestMft = NTFS_BROWSER_NAMESPACE::Test::Mft;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
using Alloc::ClusterCondition;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using TestAlloc::Freed;
using TestAlloc::Live;
using TestAlloc::Stream;
using TestFakeImage::FakeAccountFile;
using TestFakeImage::FakeAccountStream;
using TestFakeImage::FakeDataRun;

namespace {

// The record of the freed file the tests judge. It is a slot the MftTree base
// leaves to the tests, like the two live files below.
constexpr ULONGLONG subject = 22;
constexpr ULONGLONG live_owner = 17;
constexpr ULONGLONG second_live_owner = 18;
// Bytes of one cluster of the fixtures.
constexpr ULONGLONG cluster = TestFakeImage::fake_cluster_size;

}  // namespace

TEST_CASE("a never written range joins across the changes of the bitmap",
          "[alloc][cluster][sweep]") {
  TestAlloc::ForEachStrategy([](auto strategy) {
    TestAlloc::Volume volume;
    FakeAccountFile file = Freed(100, Stream({{40, 4}}, 4 * cluster));
    file.streams.front().ini_size = 0;
    volume.Put(subject, file);
    // The bitmap flips twice inside the range.
    volume.allocated = {FakeDataRun{.lcn = 41, .clusters = 1}};
    const TestMft::ScannedVolume<decltype(strategy)::value> scanned(
        volume.Build(), TestAlloc::DeletedOptions(),
        TestAlloc::Scan(Alloc::ClusterAccountLevel::CountsAndMap));

    const Alloc::ClusterAccount& account =
        TestAlloc::AccountOf(scanned.tree, subject);
    CHECK(account.tally.Count(ClusterCondition::NeverWritten) == 4);
    const auto& pieces = account.streams.at(0).pieces;
    REQUIRE(pieces.size() == 1);
    CHECK(pieces.at(0).clusters == 4);
  });
}

TEST_CASE("the sample picks the stretch with the lowest VCN",
          "[alloc][cluster][sweep]") {
  TestAlloc::ForEachStrategy([](auto strategy) {
    TestAlloc::Volume volume;
    // VCN 0 and 1 at a high LCN, VCN 2 and 3 at a low one.
    volume.Put(subject, Freed(100, Stream({{50, 2}, {40, 2}}, 4 * cluster)));
    const TestMft::ScannedVolume<decltype(strategy)::value> scanned(
        volume.Build(), TestAlloc::DeletedOptions(),
        TestAlloc::Scan(Alloc::ClusterAccountLevel::Counts,
                        Alloc::DataCheckMode::Sample));

    const Alloc::ClusterAccount& account =
        TestAlloc::AccountOf(scanned.tree, subject);
    // The sample read the zeros at LCN 50, the first cluster of the file.
    CHECK(account.tally.Count(ClusterCondition::Zeroed) == 1);
    CHECK(account.tally.Count(ClusterCondition::Intact) == 3);
  });
}

TEST_CASE("clusters the content check cannot read stay presumed intact",
          "[alloc][cluster][sweep]") {
  TestAlloc::ForEachStrategy([](auto strategy) {
    constexpr auto strategy_value = decltype(strategy)::value;
    TestAlloc::Volume volume;
    volume.Put(subject, Freed(100, Stream({{40, 4}}, 4 * cluster)));
    auto reader =
        std::make_unique<TestSupport::ScriptedDiskReader>(volume.Build());
    reader->FaultRange(40 * cluster, 44 * cluster,
                       TestSupport::ScriptedDiskReader::Fault::Fail);
    const NtfsVolume<strategy_value> opened(std::move(reader),
                                            TestAlloc::DeletedOptions());
    REQUIRE(opened.IsVolumeOK());

    const Mft::MftTree tree(
        opened, TestAlloc::Scan(Alloc::ClusterAccountLevel::CountsAndMap,
                                Alloc::DataCheckMode::Full));

    const Alloc::ClusterAccount& account = TestAlloc::AccountOf(tree, subject);
    CHECK(account.tally.Count(ClusterCondition::Intact) == 4);
    CHECK(account.tally.Count(ClusterCondition::Zeroed) == 0);
    CHECK(account.tally.Count(ClusterCondition::IntactChecked) == 0);
  });
}

#ifdef NTFS_BROWSER_ENABLE_DECOMPRESSION
TEST_CASE("two damaged clusters of one unit break it once",
          "[alloc][cluster][sweep]") {
  TestAlloc::ForEachStrategy([](auto strategy) {
    TestAlloc::Volume volume;
    // Two units of four clusters.
    FakeAccountStream stream = Stream({{40, 8}}, 8 * cluster);
    stream.comp_unit_size = TestFakeImage::compression_unit_size_shift;
    volume.Put(subject, Freed(100, stream));
    // Live files rewrote the first and the third cluster of the second unit.
    volume.Put(live_owner, Live(Stream({{44, 1}}, cluster)));
    volume.Put(second_live_owner, Live(Stream({{46, 1}}, cluster)));
    volume.allocated = {FakeDataRun{.lcn = 44, .clusters = 1},
                        FakeDataRun{.lcn = 46, .clusters = 1}};
    const TestMft::ScannedVolume<decltype(strategy)::value> scanned(
        volume.Build(), TestAlloc::DeletedOptions(),
        TestAlloc::Scan(Alloc::ClusterAccountLevel::CountsAndMap));

    const Alloc::ClusterAccount& account =
        TestAlloc::AccountOf(scanned.tree, subject);
    CHECK(account.tally.Count(ClusterCondition::Intact) == 4);
    CHECK(account.tally.Count(ClusterCondition::ReallocatedWritten) == 2);
    CHECK(account.tally.Count(ClusterCondition::LostWithUnit) == 2);
  });
}

TEST_CASE("a broken unit is found without the map of the clusters",
          "[alloc][cluster][sweep]") {
  TestAlloc::ForEachStrategy([](auto strategy) {
    TestAlloc::Volume volume;
    FakeAccountStream stream = Stream({{40, 8}}, 8 * cluster);
    stream.comp_unit_size = TestFakeImage::compression_unit_size_shift;
    volume.Put(subject, Freed(100, stream));
    volume.Put(live_owner, Live(Stream({{44, 1}}, cluster)));
    volume.allocated = {FakeDataRun{.lcn = 44, .clusters = 1}};
    const TestMft::ScannedVolume<decltype(strategy)::value> scanned(
        volume.Build(), TestAlloc::DeletedOptions(),
        TestAlloc::Scan(Alloc::ClusterAccountLevel::Counts));

    const Alloc::ClusterAccount& account =
        TestAlloc::AccountOf(scanned.tree, subject);
    CHECK(account.tally.Count(ClusterCondition::Intact) == 4);
    CHECK(account.tally.Count(ClusterCondition::ReallocatedWritten) == 1);
    CHECK(account.tally.Count(ClusterCondition::LostWithUnit) == 3);
    CHECK(account.streams.empty());
  });
}
#endif  // NTFS_BROWSER_ENABLE_DECOMPRESSION
