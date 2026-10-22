#include <ntfs-browser/win-types.h>

#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using Tests::FakeDataRun;
using Tests::FakeFreedStream;

namespace {

// Run list bytes: one run of 3 clusters at LCN 10, one of 2 at LCN 20, then a
// header byte whose length field (15 bytes) no run can have. A decoder that
// recovers keeps the first two runs and stops there.
const std::vector<BYTE> cut_after_two_runs{0x11, 0x03, 0x0A, 0x11,
                                           0x02, 0x0A, 0x1F, 0x00};

// Run list bytes: an empty run at LCN 10, then 2 clusters at LCN 20.
const std::vector<BYTE> empty_first_run{0x11, 0x00, 0x0A, 0x11,
                                        0x02, 0x0A, 0x00};

// Run list bytes: one run of 2^63 + 1 clusters at LCN 2^63 - 1. Their sum is
// 2^64, which does not fit.
const std::vector<BYTE> overflowing_run{0x88, 0x01, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x00};

// The first VCN past 2^63: the declared bound the overflowing run needs.
constexpr ULONGLONG past_half_range = (1ULL << 63U) + 10;

// Parses the freed record the fixture puts at mft_tree_stale_child_idx, with
// the unnamed stream's extents out. nullopt when the stream was rejected.
template <Cache::Strategy S>
std::optional<Alloc::StreamExtents> ExtentsOf(std::vector<BYTE> image,
                                              bool recover) {
  const NtfsVolume<S> volume(
      std::make_unique<Tests::MemoryDiskReader>(std::move(image)),
      VolumeOptions{.include_deleted = true, .recover_errors = recover});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(Tests::mft_tree_stale_child_idx));
  [[maybe_unused]] const bool parsed = record.ParseAttrs();

  const auto* stream = record.FindStream({});
  if (stream == nullptr) {
    return std::nullopt;
  }
  return stream->GetExtents();
}

std::vector<BYTE> ImageWith(const FakeFreedStream& stream) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutFreedNonResidentRecord(image, Tests::mft_tree_stale_child_idx,
                                   stream);
  return image;
}

template <Cache::Strategy S>
void CheckResidentOwnsNoCluster() {
  const NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(
                                 Tests::BuildFakeNtfsImageWithFreedFileTree()),
                             VolumeOptions{.include_deleted = true});
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(Tests::mft_tree_deleted_file_idx));
  REQUIRE(record.ParseAttrs());

  const auto* stream = record.FindStream({});
  REQUIRE(stream != nullptr);
  const Alloc::StreamExtents extents = stream->GetExtents();
  CHECK(extents.extents.empty());
  CHECK(extents.complete);
}

template <Cache::Strategy S>
void CheckSparseRunsAreSkipped() {
  const auto extents =
      ExtentsOf<S>(ImageWith({.real_size = 9 * Tests::fake_cluster_size,
                              .runs = {FakeDataRun{.lcn = 10, .clusters = 3},
                                       FakeDataRun{.clusters = 4},
                                       FakeDataRun{.lcn = 20, .clusters = 2}}}),
                   false);
  REQUIRE(extents);
  REQUIRE(extents->extents.size() == 2);
  CHECK(extents->extents.at(0).lcn == 10);
  CHECK(extents->extents.at(0).clusters == 3);
  CHECK(extents->extents.at(1).lcn == 20);
  CHECK(extents->extents.at(1).clusters == 2);
  CHECK(extents->complete);
}

template <Cache::Strategy S>
void CheckShortRunListIsIncomplete() {
  // Declared: 6 clusters. The list that parses maps 5.
  const std::vector<BYTE> image =
      ImageWith({.real_size = 6 * Tests::fake_cluster_size,
                 .runs = {FakeDataRun{.lcn = 10, .clusters = 3},
                          FakeDataRun{.lcn = 20, .clusters = 2},
                          FakeDataRun{.lcn = 30, .clusters = 1}},
                 .raw_runs = cut_after_two_runs});

  SECTION("recover mode keeps the runs before the damage") {
    const auto extents = ExtentsOf<S>(image, true);
    REQUIRE(extents);
    CHECK(extents->extents.size() == 2);
    CHECK_FALSE(extents->complete);
  }

  SECTION("strict mode rejects the attribute") {
    CHECK_FALSE(ExtentsOf<S>(image, false));
  }
}

template <Cache::Strategy S>
void CheckEmptyRunIsDamaged() {
  const auto extents = ExtentsOf<S>(
      ImageWith({.real_size = 2 * Tests::fake_cluster_size,
                 .runs = {FakeDataRun{.lcn = 10, .clusters = 2}},
                 .raw_runs = empty_first_run,
                 .last_vcn = std::numeric_limits<ULONGLONG>::max()}),
      false);
  REQUIRE(extents);
  REQUIRE(extents->extents.size() == 1);
  CHECK(extents->extents.at(0).lcn == 20);
  CHECK_FALSE(extents->complete);
}

template <Cache::Strategy S>
void CheckOverflowingRunIsDamaged() {
  const auto extents =
      ExtentsOf<S>(ImageWith({.real_size = Tests::fake_cluster_size,
                              .runs = {FakeDataRun{.lcn = 10, .clusters = 1}},
                              .raw_runs = overflowing_run,
                              .last_vcn = past_half_range}),
                   false);
  REQUIRE(extents);
  REQUIRE(extents->extents.size() == 1);
  CHECK_FALSE(extents->complete);
}

}  // namespace

TEST_CASE("a resident attribute owns no cluster", "[attr][extents]") {
  CheckResidentOwnsNoCluster<Cache::Strategy::NoCache>();
  CheckResidentOwnsNoCluster<Cache::Strategy::FullCache>();
}

TEST_CASE("sparse runs are not extents", "[attr][extents]") {
  CheckSparseRunsAreSkipped<Cache::Strategy::NoCache>();
  CheckSparseRunsAreSkipped<Cache::Strategy::FullCache>();
}

TEST_CASE("a run list shorter than the allocated size is incomplete",
          "[attr][extents]") {
  CheckShortRunListIsIncomplete<Cache::Strategy::NoCache>();
  CheckShortRunListIsIncomplete<Cache::Strategy::FullCache>();
}

TEST_CASE("an empty run makes the extents incomplete", "[attr][extents]") {
  CheckEmptyRunIsDamaged<Cache::Strategy::NoCache>();
  CheckEmptyRunIsDamaged<Cache::Strategy::FullCache>();
}

TEST_CASE("a run whose end overflows 64 bits makes the extents incomplete",
          "[attr][extents]") {
  CheckOverflowingRunIsDamaged<Cache::Strategy::NoCache>();
  CheckOverflowingRunIsDamaged<Cache::Strategy::FullCache>();
}
