#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-failure-support.h"
#include "alloc-test-support.h"
#include "alloc/cluster-bitmap.h"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::ClusterState;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Support::ThrowingCallback;
using Tests::FakeBitmapOptions;
using Tests::FakeBitmapShape;
using Tests::FakeDataRun;

namespace {

// The message of a bitmap that cannot be used at all.
constexpr std::string_view unusable_message = "$Bitmap is not usable";

// The message of a bitmap that lost a window.
constexpr std::string_view partial_message = "$Bitmap is partly unreadable";

// The message of a bitmap shorter than the volume.
constexpr std::string_view short_message = "$Bitmap is shorter than the volume";

// Clusters of the volume the cut and window tests build: past the 8192 clusters
// one cluster of bitmap tracks, so the bitmap needs a second cluster.
constexpr ULONGLONG two_cluster_volume = 8300;

// The first cluster of the bitmap data in such an image: the volume, rounded up
// to a 64 KiB cache block of 64 clusters.
constexpr ULONGLONG two_cluster_data_lcn = 8320;

// Bytes of a bitmap window in the window tests: one cluster of bitmap.
constexpr size_t one_cluster_window = Tests::fake_cluster_size;

// Fails every read that touches [first, last), and forwards the others.
class FailingRangeReader final : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  FailingRangeReader(std::vector<BYTE> image, ULONGLONG first, ULONGLONG last)
      : inner_(std::move(image)), first_(first), last_(last) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    const auto begin = static_cast<ULONGLONG>(addr.QuadPart);
    if (begin < last_ && begin + dest.size() > first_) {
      return false;
    }
    return inner_.ReadInto(addr, dest);
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    return inner_.Size();
  }

 private:
  Tests::MemoryDiskReader inner_;
  ULONGLONG first_;
  ULONGLONG last_;
};

// The freed tree, with a bitmap marking the clusters 40 and 41.
std::vector<BYTE> TreeWithBitmap(const FakeBitmapOptions& options = {}) {
  const std::vector<FakeDataRun> allocated{
      FakeDataRun{.lcn = 40, .clusters = 2}};
  return Support::WithBitmap(Tests::BuildFakeNtfsImageWithFreedFileTree(),
                             allocated, options);
}

// Checks that a volume with no usable bitmap answers unknown to everything. It
// asks for cluster 40 and for the range first and count.
template <Cache::Strategy S>
void CheckEveryAnswerIsUnknown(const NtfsVolume<S>& volume, ULONGLONG first,
                               ULONGLONG count) {
  CHECK_NOTHROW(volume.GetClusterState(40));
  CHECK(volume.GetClusterState(40) == ClusterState::Unknown);
  CHECK(volume.GetRangeState(first, count) == ClusterState::Unknown);
  CHECK_FALSE(volume.GetSpaceSummary());
}

template <Cache::Strategy S>
void CheckNothingIsReadBeforeAQuery() {
  const std::vector<BYTE> image = TreeWithBitmap();
  const auto opened = Support::Open<S>(image);
  const auto record = Support::ParsedRecord(
      *opened.volume, static_cast<ULONGLONG>(Tests::mft_tree_docs_idx));

  CHECK(opened.counter->ReadsInRange(Support::bitmap_data_offset,
                                     image.size()) == 0);
  CHECK(opened.volume->GetClusterState(40) == ClusterState::Allocated);
  CHECK(opened.counter->ReadsInRange(Support::bitmap_data_offset,
                                     image.size()) > 0);
}

template <Cache::Strategy S>
void CheckAMissingBitmapIsUnknownAndLoggedOnce(bool recover,
                                               Log::Level expected) {
  // Record 6 is zero-filled in the freed tree.
  const Support::CapturedLevels log;
  const auto opened = Support::Open<S>(
      Tests::BuildFakeNtfsImageWithFreedFileTree(),
      VolumeOptions{.include_deleted = true, .recover_errors = recover});

  CheckEveryAnswerIsUnknown(*opened.volume, 0, 10);

  CHECK(log.Count(unusable_message) == 1);
  CHECK(log.LevelOf(unusable_message) == expected);
}

template <Cache::Strategy S>
void CheckACutRunList() {
  // A volume wide enough for a bitmap of two clusters, whose run list stops
  // after the first.
  const std::vector<BYTE> image =
      TreeWithBitmap({.shape = FakeBitmapShape::CutRunList,
                      .volume_clusters = two_cluster_volume});

  SECTION("strict mode rejects the whole bitmap") {
    const auto opened = Support::Open<S>(image);

    CHECK(opened.volume->GetClusterState(40) == ClusterState::Unknown);
    CHECK_FALSE(opened.volume->GetSpaceSummary());
  }

  SECTION("recover mode answers the clusters before the cut") {
    const auto opened = Support::Open<S>(
        image, VolumeOptions{.include_deleted = true, .recover_errors = true});

    CHECK(opened.volume->GetClusterState(39) == ClusterState::Free);
    CHECK(opened.volume->GetClusterState(40) == ClusterState::Allocated);
    CHECK(opened.volume->GetClusterState(8250) == ClusterState::Unknown);
  }
}

void CheckAFailedWindowOnly() {
  const std::vector<BYTE> image =
      TreeWithBitmap({.shape = FakeBitmapShape::NonResident,
                      .volume_clusters = two_cluster_volume});
  const ULONGLONG second_cluster =
      (two_cluster_data_lcn + 1) * Tests::fake_cluster_size;

  const Support::CapturedLevels log;
  Alloc::SetBitmapWindowBytesForTests(one_cluster_window);
  auto counter = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<FailingRangeReader>(
          image, second_cluster, second_cluster + Tests::fake_cluster_size));
  Tests::CountingDiskReader* const reads = counter.get();
  const NtfsVolume<Cache::Strategy::NoCache> volume(
      std::move(counter), VolumeOptions{.include_deleted = true});
  REQUIRE(volume.IsVolumeOK());

  CHECK(volume.GetClusterState(40) == ClusterState::Allocated);
  CHECK(volume.GetClusterState(8200) == ClusterState::Unknown);
  const size_t after_first = reads->ReadsInRange(
      second_cluster, second_cluster + Tests::fake_cluster_size);
  CHECK(after_first > 0);

  CHECK(volume.GetClusterState(8201) == ClusterState::Unknown);
  CHECK(volume.GetClusterState(41) == ClusterState::Allocated);
  CHECK(reads->ReadsInRange(second_cluster,
                            second_cluster + Tests::fake_cluster_size) ==
        after_first);
  CHECK(log.Count(partial_message) == 1);

  Alloc::SetBitmapWindowBytesForTests(0);
}

template <Cache::Strategy S>
void CheckAShortBitmapLogsOnce() {
  const std::array<BYTE, 4> four_bytes{0x00, 0x00, 0x00, 0x00};
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  Tests::PutClusterBitmap(
      image, four_bytes,
      {.shape = FakeBitmapShape::Resident, .pad_to_volume = false});

  const Support::CapturedLevels log;
  const auto opened = Support::Open<S>(image);

  CHECK(opened.volume->GetClusterState(10) == ClusterState::Free);
  CHECK(opened.volume->GetClusterState(40) == ClusterState::Unknown);
  CHECK(opened.volume->GetClusterState(41) == ClusterState::Unknown);
  CHECK(log.Count(short_message) == 1);
}

template <Cache::Strategy S>
void CheckAThrowingCallbackIsContained() {
  const auto opened = Support::Open<S>(TreeWithBitmap());
  REQUIRE(opened.volume->InstallAttrRawCB(Attr::Type::Data, &ThrowingCallback));

  CheckEveryAnswerIsUnknown(*opened.volume, 40, 2);
}

}  // namespace

TEST_CASE("nothing reads the bitmap before a query needs it",
          "[alloc][failure]") {
  CheckNothingIsReadBeforeAQuery<Cache::Strategy::NoCache>();
  CheckNothingIsReadBeforeAQuery<Cache::Strategy::FullCache>();
}

TEST_CASE("a missing bitmap makes every answer unknown and is logged once",
          "[alloc][failure]") {
  SECTION("strict mode warns") {
    CheckAMissingBitmapIsUnknownAndLoggedOnce<Cache::Strategy::NoCache>(
        false, Log::Level::Warn);
    CheckAMissingBitmapIsUnknownAndLoggedOnce<Cache::Strategy::FullCache>(
        false, Log::Level::Warn);
  }

  SECTION("recover mode only informs") {
    CheckAMissingBitmapIsUnknownAndLoggedOnce<Cache::Strategy::NoCache>(
        true, Log::Level::Info);
    CheckAMissingBitmapIsUnknownAndLoggedOnce<Cache::Strategy::FullCache>(
        true, Log::Level::Info);
  }
}

TEST_CASE("a bitmap whose run list is cut", "[alloc][failure]") {
  SECTION("with the no cache strategy") {
    CheckACutRunList<Cache::Strategy::NoCache>();
  }

  SECTION("with the full cache strategy") {
    CheckACutRunList<Cache::Strategy::FullCache>();
  }
}

TEST_CASE("a window that cannot be read spoils only itself, once",
          "[alloc][failure]") {
  CheckAFailedWindowOnly();
}

TEST_CASE("a bitmap shorter than the volume is logged once",
          "[alloc][failure]") {
  CheckAShortBitmapLogsOnce<Cache::Strategy::NoCache>();
  CheckAShortBitmapLogsOnce<Cache::Strategy::FullCache>();
}

TEST_CASE("a callback that throws while the bitmap loads is contained",
          "[alloc][failure]") {
  CheckAThrowingCallbackIsContained<Cache::Strategy::NoCache>();
  CheckAThrowingCallbackIsContained<Cache::Strategy::FullCache>();
}
