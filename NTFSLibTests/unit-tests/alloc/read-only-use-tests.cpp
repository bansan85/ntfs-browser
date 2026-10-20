#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "alloc-test-support.h"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Tests::CountingDiskReader;
using Tests::FakeDataRun;

namespace {

using Kind = CountingDiskReader::Kind;

template <Cache::Strategy S>
void CheckEveryQueryOnlyReads() {
  const std::vector<FakeDataRun> allocated{
      FakeDataRun{.lcn = 40, .clusters = 2}};
  const auto opened = Support::Open<S>(
      Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
                              4 * Tests::fake_cluster_size,
                              {FakeDataRun{.lcn = 40, .clusters = 4}}),
                          allocated));

  CHECK(opened.volume->GetClusterState(40) == Alloc::ClusterState::Allocated);
  CHECK(opened.volume->GetRangeState(30, 20) == Alloc::ClusterState::Allocated);
  std::vector<Alloc::RangeQuery> queries{{.lcn = 10, .clusters = 5},
                                         {.lcn = 41, .clusters = 1}};
  opened.volume->GetRangeStates(queries);
  CHECK(opened.volume->GetSpaceSummary());
  const auto record =
      Support::ParsedRecord(*opened.volume, Tests::mft_tree_stale_child_idx);
  CHECK(record->GetDataState() == Alloc::DataState::Reallocated);

  const auto& calls = opened.counter->Calls();
  REQUIRE_FALSE(calls.empty());
  CHECK(std::ranges::all_of(calls, [](const CountingDiskReader::Call& call) {
    return call.kind == Kind::Open || call.kind == Kind::Read ||
           call.kind == Kind::Size;
  }));
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
