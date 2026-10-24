#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "alloc-test-support.h"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfs-browser/disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Tests::CountingDiskReader;
using Tests::FakeDataRun;

namespace {

using Kind = CountingDiskReader::Kind;

// The freed file over four clusters at LCN 40.
std::vector<BYTE> StaleFile() {
  return Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
      4 * Tests::fake_cluster_size, {FakeDataRun{.lcn = 40, .clusters = 4}});
}

// Runs the range, summary and data state queries on the volume of StaleFile(),
// then checks that its reader saw nothing but opens, reads and sizes.
template <Cache::Strategy S>
void CheckTheRestOnlyReads(const Support::Opened<S>& opened) {
  CHECK(opened.volume->GetRangeState(30, 20) == Alloc::ClusterState::Allocated);
  std::vector<Alloc::RangeQuery> queries{{.lcn = 10, .clusters = 5},
                                         {.lcn = 41, .clusters = 1}};
  opened.volume->GetRangeStates(queries);
  CHECK(opened.volume->GetSpaceSummary());
  CHECK(Support::ParsedStale(opened)->GetDataState() ==
        Alloc::DataState::Reallocated);

  const auto& calls = opened.counter->Calls();
  REQUIRE_FALSE(calls.empty());
  CHECK(std::ranges::all_of(calls, [](const CountingDiskReader::Call& call) {
    return call.kind == Kind::Open || call.kind == Kind::Read ||
           call.kind == Kind::Size || call.kind == Kind::Prefetch ||
           call.kind == Kind::CancelPrefetch;
  }));
}

template <Cache::Strategy S>
void CheckEveryQueryOnlyReads() {
  const std::vector<FakeDataRun> allocated{
      FakeDataRun{.lcn = 40, .clusters = 2}};
  const auto opened =
      Support::OpenVolume<S>(Support::WithBitmap(StaleFile(), allocated));

  CHECK(opened.volume->GetClusterState(40) == Alloc::ClusterState::Allocated);
  CheckTheRestOnlyReads(opened);
}

template <Cache::Strategy S>
void CheckEveryBadClusterQueryOnlyReads() {
  const std::vector<FakeDataRun> allocated{
      FakeDataRun{.lcn = 40, .clusters = 2}};
  const std::vector<Alloc::Extent> bad{{.lcn = 41, .clusters = 1}};
  const auto opened = Support::OpenVolume<S>(Support::WithBadClusters(
      Support::WithBitmap(StaleFile(), Support::WithBadRuns(allocated, bad)),
      bad));

  REQUIRE(opened.volume->GetBadClusters() != nullptr);
  CHECK(opened.volume->GetClusterState(41) == Alloc::ClusterState::Bad);
  CheckTheRestOnlyReads(opened);
}

}  // namespace

TEST_CASE("the counting reader records open, read and size calls",
          "[alloc][read-only]") {
  CountingDiskReader reader(
      std::make_unique<Tests::MemoryDiskReader>(std::vector<BYTE>(4096, 0x5A)));
  std::vector<BYTE> out(16);
  LARGE_INTEGER addr{.QuadPart = 128};

  CHECK(reader.ReadInto(addr, out));
  CHECK(reader.Size() == 4096);
  REQUIRE(reader.Calls().size() == 2);
  CHECK(reader.Calls().at(0).kind == Kind::Read);
  CHECK(reader.Calls().at(0).offset == 128);
  CHECK(reader.Calls().at(0).length == 16);
  CHECK(reader.Calls().at(1).kind == Kind::Size);
  CHECK(reader.ReadCount() == 1);
  CHECK(reader.ReadsInRange(100, 140) == 1);
  CHECK(reader.ReadsInRange(144, 200) == 0);

  reader.Reset();
  CHECK(reader.Calls().empty());
}

TEST_CASE("every allocation query only reads", "[alloc][read-only]") {
  CheckEveryQueryOnlyReads<Cache::Strategy::NoCache>();
  CheckEveryQueryOnlyReads<Cache::Strategy::FullCache>();
}

TEST_CASE("every bad cluster query only reads", "[alloc][read-only]") {
  CheckEveryBadClusterQueryOnlyReads<Cache::Strategy::NoCache>();
  CheckEveryBadClusterQueryOnlyReads<Cache::Strategy::FullCache>();
}
