#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/types.h>

#include "alloc/types.h"
#include "scan-support.h"
#include "win-types.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestAlloc = NTFS_BROWSER_NAMESPACE::Test::Alloc;
namespace TestFake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE;
using TestSearch::RunScan;
using TestSearch::TempImage;

namespace {

// Slots of the indexed fixture: 16 system records and 48 more.
constexpr ULONGLONG indexed_records = 64;

// The freed stale file of the fixture below: four clusters at LCN 40, one of
// them bad.
const std::vector<TestFake::FakeDataRun> stale_runs{
    TestFake::FakeDataRun{.lcn = 40, .clusters = 4}};
const std::vector<Lib::Alloc::Extent> one_bad{{.lcn = 41, .clusters = 1}};

// A volume whose freed file old.tmp, which the index of Docs names, lies on a
// bad cluster.
std::vector<BYTE> BadClusterImage() {
  std::vector<BYTE> image = TestFake::BuildFakeNtfsImageWithFreedFileTree();
  TestFake::PutFreedNonResidentRecord(
      image, TestFake::mft_tree_deleted_file_idx,
      {.real_size = TestAlloc::stale_size, .runs = stale_runs});
  image = TestAlloc::WithBitmap(std::move(image),
                                TestAlloc::WithBadRuns({}, one_bad));
  return TestAlloc::WithBadClusters(std::move(image), one_bad);
}

// The row of the record, or a row with no name.
TestSearch::Row RowOfRecord(const std::vector<TestSearch::Row>& rows,
                            std::uint64_t record) {
  for (const TestSearch::Row& row : rows) {
    if (row.record == record) {
      return row;
    }
  }
  return {};
}

// The standings of the rows that carry one.
bool HasStanding(const std::vector<TestSearch::Row>& rows,
                 Lib::Standing standing) {
  return std::ranges::any_of(rows, [standing](const TestSearch::Row& row) {
    return row.standing == standing;
  });
}

}  // namespace

TEST_CASE("A folder scan tells the entries that name a reused record",
          "[search][scan]") {
  const TempImage image(
      TestFake::BuildFakeNtfsImageWithIndexedMftScan(indexed_records));
  const auto engine = GENERATE(Lib::EngineKind::DirectoryNoCache,
                               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options;
  options.target = image.Target();
  options.engine = engine;
  options.include_deleted = true;
  options.recover_errors = true;

  const auto outcome = RunScan(options);

  CHECK(outcome.state == Lib::State::Finished);
  CHECK(HasStanding(outcome.rows, Lib::Standing::Live));
  CHECK(HasStanding(outcome.rows, Lib::Standing::Reused));
  CHECK(HasStanding(outcome.rows, Lib::Standing::Unreadable));
}

TEST_CASE("A scan tells a deleted file that lies on a bad cluster",
          "[search][scan]") {
  const TempImage image(BadClusterImage());
  const auto engine =
      GENERATE(Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache);
  Lib::Options options;
  options.target = image.Target();
  options.engine = engine;
  options.recursive = true;
  options.include_deleted = true;
  options.recover_errors = true;

  const auto outcome = RunScan(options);

  const auto stale =
      RowOfRecord(outcome.rows, TestFake::mft_tree_deleted_file_idx);
  REQUIRE_FALSE(stale.name.empty());
  CHECK(stale.data_state == Lib::DataState::Bad);
}
