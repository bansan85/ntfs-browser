#include <ntfs-browser/win-types.h>

#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>

#include "alloc-test-support.h"
#include "alloc/content-check.h"
#include "cluster-account-support.h"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "mft-tree-state-support.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace AllocSupport = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AccountSupport;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace TreeSupport = NTFS_BROWSER_NAMESPACE::Tests::MftTreeSupport;
using Alloc::ClusterCondition;
using Alloc::DataCheckMode;
using Support::CountOf;
using Support::Freed;
using Support::Stream;
using Tests::FakeAccountFile;
using Tests::FakeDataRun;

namespace {

constexpr ULONGLONG subject = 22;
constexpr ULONGLONG cluster = Tests::fake_cluster_size;
constexpr ULONGLONG first_cluster = 40;

// Writes bytes at the start of cluster lcn of image.
void PutAt(std::vector<BYTE>& image, ULONGLONG lcn, size_t offset,
           std::string_view bytes) {
  std::memcpy(&image.at((lcn * cluster) + offset), bytes.data(), bytes.size());
}

// Fills cluster lcn of image with a pattern that is neither zeros nor
// metadata.
void FillWithData(std::vector<BYTE>& image, ULONGLONG lcn) {
  for (size_t i = 0; i < cluster; i++) {
    image.at((lcn * cluster) + i) = static_cast<BYTE>((i % 251) + 1);
  }
}

// The subject: four clusters at 40 to 43, of which the first is zeros, the
// second starts a file record in its second sector, and the others are data.
Support::Volume Subject() {
  Support::Volume volume;
  volume.Put(subject, Freed(100, Stream({{first_cluster, 4}}, 4 * cluster)));
  PutAt(volume.image, 41, 512, "FILE");
  FillWithData(volume.image, 42);
  FillWithData(volume.image, 43);
  return volume;
}

template <Cache::Strategy S>
TreeSupport::ScannedVolume<S> Check(const Support::Volume& volume,
                                    DataCheckMode mode) {
  return TreeSupport::ScannedVolume<S>(
      volume.Build(), AllocSupport::DeletedOptions(),
      Support::Scan(Alloc::ClusterAccountLevel::CountsAndMap, mode));
}

}  // namespace

TEST_CASE("a cluster is classified by what it holds",
          "[alloc][cluster][check]") {
  std::array<BYTE, 1024> bytes{};
  CHECK(Alloc::ClassifyCluster(bytes, 512) == Alloc::ClusterContent::Zeros);

  bytes.at(1000) = 1;
  CHECK(Alloc::ClassifyCluster(bytes, 512) == Alloc::ClusterContent::Data);

  for (const char* signature : {"FILE", "INDX", "RCRD", "RSTR"}) {
    bytes.fill(0);
    std::memcpy(bytes.data() + 512, signature, 4);
    CHECK(Alloc::ClassifyCluster(bytes, 512) ==
          Alloc::ClusterContent::Metadata);
  }

  // A signature in the middle of a sector is data.
  bytes.fill(0);
  std::memcpy(bytes.data() + 100, "FILE", 4);
  CHECK(Alloc::ClassifyCluster(bytes, 512) == Alloc::ClusterContent::Data);
  // A cluster of 512-byte sectors checked as such by default.
  bytes.fill(0);
  std::memcpy(bytes.data() + 512, "BAAD", 4);
  CHECK(Alloc::ClassifyCluster(bytes, 0) == Alloc::ClusterContent::Data);
}

TEST_CASE("the full content check tells zeros, metadata and data",
          "[alloc][cluster][check]") {
  Support::ForEachStrategy([](auto strategy) {
    const auto scanned =
        Check<decltype(strategy)::value>(Subject(), DataCheckMode::Full);

    const Alloc::ClusterAccount& account =
        Support::AccountOf(scanned.tree, subject);
    CHECK(account.tally.Count(ClusterCondition::Zeroed) == 1);
    CHECK(account.tally.Count(ClusterCondition::OverwrittenByMetadata) == 1);
    CHECK(account.tally.Count(ClusterCondition::IntactChecked) == 2);
    CHECK(account.tally.Count(ClusterCondition::Intact) == 0);

    const auto& pieces = account.streams.at(0).pieces;
    REQUIRE(pieces.size() == 3);
    CHECK(pieces.at(0).condition == ClusterCondition::Zeroed);
    CHECK(pieces.at(1).condition == ClusterCondition::OverwrittenByMetadata);
    CHECK(pieces.at(2).condition == ClusterCondition::IntactChecked);
    CHECK(pieces.at(2).clusters == 2);
  });
}

TEST_CASE("the sample reads the first cluster of each stream",
          "[alloc][cluster][check]") {
  Support::ForEachStrategy([](auto strategy) {
    const auto scanned =
        Check<decltype(strategy)::value>(Subject(), DataCheckMode::Sample);

    const Alloc::ClusterAccount& account =
        Support::AccountOf(scanned.tree, subject);
    CHECK(account.tally.Count(ClusterCondition::Zeroed) == 1);
    CHECK(account.tally.Count(ClusterCondition::Intact) == 3);
    CHECK(account.tally.Count(ClusterCondition::IntactChecked) == 0);
  });
}

TEST_CASE("the content check implies the counts", "[alloc][cluster][check]") {
  Support::ForEachStrategy([](auto strategy) {
    const TreeSupport::ScannedVolume<decltype(strategy)::value> scanned(
        Subject().Build(), AllocSupport::DeletedOptions(),
        Support::Scan(Alloc::ClusterAccountLevel::Off, DataCheckMode::Full));

    CHECK(CountOf(scanned.tree, subject, ClusterCondition::Zeroed) == 1);
    CHECK(Support::AccountOf(scanned.tree, subject).streams.empty());
  });
}

TEST_CASE("the content check skips an encrypted stream, and what is not intact",
          "[alloc][cluster][check]") {
  Support::ForEachStrategy([](auto strategy) {
    Support::Volume volume = Subject();
    Tests::FakeAccountStream encrypted =
        Stream({{first_cluster, 4}}, 4 * cluster);
    encrypted.encrypted = true;
    volume.Put(subject, Freed(100, encrypted));
    const auto scanned =
        Check<decltype(strategy)::value>(volume, DataCheckMode::Full);

    CHECK(CountOf(scanned.tree, subject, ClusterCondition::Intact) == 4);
    CHECK(CountOf(scanned.tree, subject, ClusterCondition::Zeroed) == 0);

    // A bad cluster is not read: the check keeps to what it presumed intact.
    Support::Volume bad = Subject();
    bad.bad = {Alloc::Extent{.lcn = 40, .clusters = 1}};
    bad.allocated = {FakeDataRun{.lcn = 40, .clusters = 1}};
    const auto with_bad =
        Check<decltype(strategy)::value>(bad, DataCheckMode::Full);
    CHECK(CountOf(with_bad.tree, subject, ClusterCondition::Bad) == 1);
    CHECK(CountOf(with_bad.tree, subject, ClusterCondition::Zeroed) == 0);
  });
}

TEST_CASE("the content check reads in order, announced, and nothing else",
          "[alloc][cluster][check]") {
  const Support::Volume volume = Subject();
  const std::vector<BYTE> image = volume.Build();
  const auto data_first = first_cluster * cluster;
  const auto data_last = (first_cluster + 4) * cluster;

  const TreeSupport::ScannedVolume<Cache::Strategy::NoCache> counts(
      image, AllocSupport::DeletedOptions(), Support::Scan());
  CHECK(counts.opened.counter->ReadsInRange(data_first, data_last) == 0);

  const TreeSupport::ScannedVolume<Cache::Strategy::NoCache> full(
      image, AllocSupport::DeletedOptions(),
      Support::Scan(Alloc::ClusterAccountLevel::Counts, DataCheckMode::Full));
  CHECK(full.opened.counter->ReadsInRange(data_first, data_last) == 1);

  // The hint comes before the read it announces.
  bool announced = false;
  for (const Tests::CountingDiskReader::Call& call :
       full.opened.counter->Calls()) {
    if (call.kind == Tests::CountingDiskReader::Kind::Prefetch &&
        call.offset == data_first) {
      announced = true;
    }
    if (call.kind == Tests::CountingDiskReader::Kind::Read &&
        call.offset == data_first) {
      CHECK(announced);
    }
  }
  CHECK(announced);
}
