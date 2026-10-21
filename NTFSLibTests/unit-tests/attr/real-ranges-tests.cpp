#include <ntfs-browser/win-types.h>

#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "attr/non-resident.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
using Tests::FakeDataRun;
using Tests::FakeFreedStream;

namespace {

constexpr ULONGLONG cluster_bytes = Tests::fake_cluster_size;

// Run list bytes: one run of 3 clusters at LCN 10, one of 2 at LCN 20, then a
// header byte whose length field (15 bytes) no run can have.
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

constexpr ULONGLONG past_half_range = (1ULL << 63U) + 10;

// Run list bytes: a sparse run of 2^62 clusters, then one real cluster at LCN
// 10. The real run starts at a VCN whose byte offset does not fit 64 bits.
const std::vector<BYTE> far_real_run{0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x40, 0x11, 0x01, 0x0A, 0x00};

// Last VCN that holds the real run of far_real_run.
constexpr ULONGLONG far_real_run_last_vcn = (1ULL << 62U) + 1;

template <Cache::Strategy S>
std::optional<Alloc::StreamRanges> RangesOf(std::vector<BYTE> image,
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
  return stream->GetRealRanges();
}

std::vector<BYTE> ImageWith(const FakeFreedStream& stream) {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutFreedNonResidentRecord(image, Tests::mft_tree_stale_child_idx,
                                   stream);
  return image;
}

template <Cache::Strategy S>
void CheckSparseHead() {
  const auto ranges =
      RangesOf<S>(ImageWith({.real_size = 6 * cluster_bytes,
                             .runs = {FakeDataRun{.clusters = 4},
                                      FakeDataRun{.lcn = 20, .clusters = 2}}}),
                  false);
  REQUIRE(ranges);
  REQUIRE(ranges->ranges.size() == 1);
  CHECK(ranges->ranges.at(0).offset == 4 * cluster_bytes);
  CHECK(ranges->ranges.at(0).length == 2 * cluster_bytes);
  CHECK(ranges->complete);
}

template <Cache::Strategy S>
void CheckSparseHoleInTheMiddle() {
  const auto ranges =
      RangesOf<S>(ImageWith({.real_size = 9 * cluster_bytes,
                             .runs = {FakeDataRun{.lcn = 10, .clusters = 3},
                                      FakeDataRun{.clusters = 4},
                                      FakeDataRun{.lcn = 20, .clusters = 2}}}),
                  false);
  REQUIRE(ranges);
  REQUIRE(ranges->ranges.size() == 2);
  CHECK(ranges->ranges.at(0).offset == 0);
  CHECK(ranges->ranges.at(0).length == 3 * cluster_bytes);
  CHECK(ranges->ranges.at(1).offset == 7 * cluster_bytes);
  CHECK(ranges->ranges.at(1).length == 2 * cluster_bytes);
  CHECK(ranges->complete);
}

template <Cache::Strategy S>
void CheckAdjacentRunsMerge() {
  const auto ranges =
      RangesOf<S>(ImageWith({.real_size = 5 * cluster_bytes,
                             .runs = {FakeDataRun{.lcn = 10, .clusters = 3},
                                      FakeDataRun{.lcn = 50, .clusters = 2}}}),
                  false);
  REQUIRE(ranges);
  REQUIRE(ranges->ranges.size() == 1);
  CHECK(ranges->ranges.at(0).offset == 0);
  CHECK(ranges->ranges.at(0).length == 5 * cluster_bytes);
}

template <Cache::Strategy S>
void CheckClampedToTheDataSize() {
  // The last cluster is allocated but past the stream's real size.
  const auto ranges =
      RangesOf<S>(ImageWith({.real_size = 4 * cluster_bytes,
                             .runs = {FakeDataRun{.lcn = 10, .clusters = 3},
                                      FakeDataRun{.lcn = 20, .clusters = 2}}}),
                  false);
  REQUIRE(ranges);
  REQUIRE(ranges->ranges.size() == 1);
  CHECK(ranges->ranges.at(0).offset == 0);
  CHECK(ranges->ranges.at(0).length == 4 * cluster_bytes);
}

template <Cache::Strategy S>
void CheckRangePastTheDataSizeIsDropped() {
  // The only real run starts after the stream's real size.
  const auto ranges =
      RangesOf<S>(ImageWith({.real_size = 2 * cluster_bytes,
                             .runs = {FakeDataRun{.clusters = 4},
                                      FakeDataRun{.lcn = 20, .clusters = 2}}}),
                  false);
  REQUIRE(ranges);
  CHECK(ranges->ranges.empty());
}

template <Cache::Strategy S>
void CheckShortRunListIsIncomplete() {
  const std::vector<BYTE> image =
      ImageWith({.real_size = 6 * cluster_bytes,
                 .runs = {FakeDataRun{.lcn = 10, .clusters = 3},
                          FakeDataRun{.lcn = 20, .clusters = 2},
                          FakeDataRun{.lcn = 30, .clusters = 1}},
                 .raw_runs = cut_after_two_runs});

  const auto ranges = RangesOf<S>(image, true);
  REQUIRE(ranges);
  REQUIRE(ranges->ranges.size() == 1);
  CHECK(ranges->ranges.at(0).length == 5 * cluster_bytes);
  CHECK_FALSE(ranges->complete);
}

template <Cache::Strategy S>
void CheckEmptyRunIsDamaged() {
  const auto ranges = RangesOf<S>(
      ImageWith({.real_size = 2 * cluster_bytes,
                 .runs = {FakeDataRun{.lcn = 10, .clusters = 2}},
                 .raw_runs = empty_first_run,
                 .last_vcn = std::numeric_limits<ULONGLONG>::max()}),
      false);
  REQUIRE(ranges);
  CHECK_FALSE(ranges->complete);
}

template <Cache::Strategy S>
void CheckOverflowingRunIsDamaged() {
  const auto ranges =
      RangesOf<S>(ImageWith({.real_size = cluster_bytes,
                             .runs = {FakeDataRun{.lcn = 10, .clusters = 1}},
                             .raw_runs = overflowing_run,
                             .last_vcn = past_half_range}),
                  false);
  REQUIRE(ranges);
  CHECK_FALSE(ranges->complete);
}

template <Cache::Strategy S>
void CheckFarRealRunIsDamaged() {
  const auto ranges =
      RangesOf<S>(ImageWith({.real_size = cluster_bytes,
                             .runs = {FakeDataRun{.lcn = 10, .clusters = 1}},
                             .raw_runs = far_real_run,
                             .last_vcn = far_real_run_last_vcn}),
                  false);
  REQUIRE(ranges);
  CHECK_FALSE(ranges->complete);
  CHECK(ranges->ranges.empty());
}

template <Cache::Strategy S>
void CheckExtentReadRefusesAnOverflowingSize() {
  const NtfsVolume<S> volume(
      std::make_unique<Tests::MemoryDiskReader>(
          ImageWith({.real_size = cluster_bytes,
                     .runs = {FakeDataRun{.lcn = 10, .clusters = 1}},
                     .raw_runs = overflowing_run,
                     .last_vcn = past_half_range})),
      VolumeOptions{.include_deleted = true});
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(Tests::mft_tree_stale_child_idx));
  REQUIRE(record.ParseAttrs());
  const auto* stream = record.FindStream({});
  REQUIRE(stream != nullptr);
  REQUIRE(stream->IsNonResident());

  std::vector<BYTE> buffer(16);
  const auto* non_resident =
      static_cast<const Attr::AttrNonResident<S>*>(stream);
  CHECK_FALSE(non_resident->ReadExtentData(0, buffer));
}

template <Cache::Strategy S>
void CheckEmptyResidentHasNoRange() {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  // $UsnJrnl without a $J stream keeps an empty, unnamed resident $DATA.
  Tests::PutUsnJournal(image, {.omit_j_stream = true});
  const NtfsVolume<S> volume(
      std::make_unique<Tests::MemoryDiskReader>(std::move(image)),
      VolumeOptions{.include_deleted = true});
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(Tests::usn_journal_idx));
  REQUIRE(record.ParseAttrs());

  const auto* stream = record.FindStream({});
  REQUIRE(stream != nullptr);
  REQUIRE(stream->GetDataSize() == 0);
  const Alloc::StreamRanges ranges = stream->GetRealRanges();
  CHECK(ranges.ranges.empty());
}

template <Cache::Strategy S>
void CheckResidentIsOneRange() {
  const NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(
                                 Tests::BuildFakeNtfsImageWithFreedFileTree()),
                             VolumeOptions{.include_deleted = true});
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(Tests::mft_tree_deleted_file_idx));
  REQUIRE(record.ParseAttrs());

  const auto* stream = record.FindStream({});
  REQUIRE(stream != nullptr);
  const Alloc::StreamRanges ranges = stream->GetRealRanges();
  REQUIRE(ranges.ranges.size() == 1);
  CHECK(ranges.ranges.at(0).offset == 0);
  CHECK(ranges.ranges.at(0).length == stream->GetDataSize());
  CHECK(ranges.complete);
}

}  // namespace

TEST_CASE("a sparse head is not a real range", "[attr][ranges]") {
  CheckSparseHead<Cache::Strategy::NoCache>();
  CheckSparseHead<Cache::Strategy::FullCache>();
}

TEST_CASE("a sparse hole splits the real ranges", "[attr][ranges]") {
  CheckSparseHoleInTheMiddle<Cache::Strategy::NoCache>();
  CheckSparseHoleInTheMiddle<Cache::Strategy::FullCache>();
}

TEST_CASE("runs that touch in the stream make one range", "[attr][ranges]") {
  CheckAdjacentRunsMerge<Cache::Strategy::NoCache>();
  CheckAdjacentRunsMerge<Cache::Strategy::FullCache>();
}

TEST_CASE("a real range is cut at the data size", "[attr][ranges]") {
  CheckClampedToTheDataSize<Cache::Strategy::NoCache>();
  CheckClampedToTheDataSize<Cache::Strategy::FullCache>();
}

TEST_CASE("a real run past the data size is dropped", "[attr][ranges]") {
  CheckRangePastTheDataSizeIsDropped<Cache::Strategy::NoCache>();
  CheckRangePastTheDataSizeIsDropped<Cache::Strategy::FullCache>();
}

TEST_CASE("a run list shorter than the allocated size is incomplete ranges",
          "[attr][ranges]") {
  CheckShortRunListIsIncomplete<Cache::Strategy::NoCache>();
  CheckShortRunListIsIncomplete<Cache::Strategy::FullCache>();
}

TEST_CASE("an empty run makes the ranges incomplete", "[attr][ranges]") {
  CheckEmptyRunIsDamaged<Cache::Strategy::NoCache>();
  CheckEmptyRunIsDamaged<Cache::Strategy::FullCache>();
}

TEST_CASE("a run whose end overflows 64 bits makes the ranges incomplete",
          "[attr][ranges]") {
  CheckOverflowingRunIsDamaged<Cache::Strategy::NoCache>();
  CheckOverflowingRunIsDamaged<Cache::Strategy::FullCache>();
}

TEST_CASE("a resident attribute is one range", "[attr][ranges]") {
  CheckResidentIsOneRange<Cache::Strategy::NoCache>();
  CheckResidentIsOneRange<Cache::Strategy::FullCache>();
}

TEST_CASE("a real run too far for a byte offset makes the ranges incomplete",
          "[attr][ranges]") {
  CheckFarRealRunIsDamaged<Cache::Strategy::NoCache>();
  CheckFarRealRunIsDamaged<Cache::Strategy::FullCache>();
}

TEST_CASE("an extent read refuses a size that overflows 64 bits",
          "[attr][ranges]") {
  CheckExtentReadRefusesAnOverflowingSize<Cache::Strategy::NoCache>();
  CheckExtentReadRefusesAnOverflowingSize<Cache::Strategy::FullCache>();
}

TEST_CASE("an empty resident attribute has no range", "[attr][ranges]") {
  CheckEmptyResidentHasNoRange<Cache::Strategy::NoCache>();
  CheckEmptyResidentHasNoRange<Cache::Strategy::FullCache>();
}
