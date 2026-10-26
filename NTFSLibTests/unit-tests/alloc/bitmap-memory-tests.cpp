#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "alloc-test-support.h"
#include "alloc/cluster-bitmap.h"
#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
using Alloc::ClusterState;
using Tests::FakeBitmapOptions;
using Tests::FakeBitmapShape;
using Tests::FakeDataRun;

namespace {

// The window these tests use: 16 bytes of bitmap, 128 clusters.
constexpr size_t tiny_window = 16;

// Clusters of the volume: four windows of 128.
constexpr ULONGLONG four_window_volume = 512;

// Restores the default window when a test ends, however it ends.
class WindowOverride {
 public:
  explicit WindowOverride(size_t bytes) {
    Alloc::SetBitmapWindowBytesForTests(bytes);
  }

  WindowOverride(const WindowOverride&) = delete;
  WindowOverride& operator=(const WindowOverride&) = delete;

  ~WindowOverride() { Alloc::SetBitmapWindowBytesForTests(0); }
};

template <Cache::Strategy S>
void CheckEveryClusterIsAnsweredFromOneWindow() {
  const WindowOverride window(tiny_window);
  const std::vector<FakeDataRun> allocated{
      FakeDataRun{.lcn = 40, .clusters = 2}};
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  const std::vector<BYTE> bits = Tests::AllocatedBitmapFor(image, allocated);
  // The volume grows to four windows before the bitmap sees it.
  Tests::PutClusterBitmap(image, bits, {.volume_clusters = four_window_volume});
  const auto opened = Support::OpenVolume<S>(image);

  size_t answered = 0;
  for (ULONGLONG lcn = 0; lcn < four_window_volume; lcn++) {
    const ClusterState state = opened.volume->GetClusterState(lcn);
    CHECK(state != ClusterState::OutOfRange);
    answered++;
  }

  CHECK(answered == four_window_volume);
  CHECK(opened.volume->GetClusterState(40) == ClusterState::Allocated);
  CHECK(Alloc::PeakBitmapBytesHeld() <= tiny_window);
  CHECK(Alloc::PeakBitmapBytesHeld() > 0);
}

// Clusters of one window: the first query of a call falls in each in turn.
constexpr ULONGLONG window_clusters = 128;

// What two calls of GetRangeStates() cost: the reads of the bitmap, and the
// most bytes of it held at once.
struct TwoCalls {
  size_t reads{0};
  size_t peak_bytes{0};
};

// The cost of two calls of GetRangeStates(), one query in each of the four
// windows per call, when the volume keeps keep_bytes of the bitmap. The bitmap
// is 16 bytes a window, in one cluster each.
TwoCalls BitmapCostOfTwoCalls(ULONGLONG keep_bytes) {
  const WindowOverride window(tiny_window);
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedFileTree();
  const std::vector<BYTE> bits = Tests::AllocatedBitmapFor(image, {});
  Tests::PutClusterBitmap(image, bits, {.volume_clusters = four_window_volume});
  const ULONGLONG end = image.size();
  const auto opened = Support::OpenVolume<Cache::Strategy::NoCache>(image);
  opened.volume->KeepBitmapWindows(keep_bytes);

  for (int call = 0; call < 2; call++) {
    std::vector<Alloc::RangeQuery> queries;
    for (ULONGLONG at = 0; at < four_window_volume; at += window_clusters) {
      queries.push_back({.lcn = at + 1, .clusters = 1});
    }
    opened.volume->GetRangeStates(queries);
    for (const Alloc::RangeQuery& query : queries) {
      CHECK(query.state == ClusterState::Free);
    }
  }
  return {.reads =
              opened.counter->ReadsInRange(Support::bitmap_data_offset, end),
          .peak_bytes = Alloc::PeakBitmapBytesHeld()};
}

}  // namespace

TEST_CASE("a volume that keeps its bitmap windows reads each of them once",
          "[alloc][memory]") {
  CHECK(BitmapCostOfTwoCalls(ULONGLONG{1} << 20).reads == 4);
  CHECK(BitmapCostOfTwoCalls(2 * tiny_window).reads == 6);
  CHECK(BitmapCostOfTwoCalls(0).reads == 8);
}

TEST_CASE("a volume that keeps nothing holds one window of its bitmap",
          "[alloc][memory]") {
  const size_t peak = BitmapCostOfTwoCalls(0).peak_bytes;
  CHECK(peak <= tiny_window);
  CHECK(peak > 0);
}

TEST_CASE("a volume that keeps its bitmap windows holds them all",
          "[alloc][memory]") {
  CHECK(BitmapCostOfTwoCalls(ULONGLONG{1} << 20).peak_bytes == 4 * tiny_window);
  CHECK(BitmapCostOfTwoCalls(2 * tiny_window).peak_bytes <= 3 * tiny_window);
}

TEST_CASE("a bitmap of several windows is never held whole",
          "[alloc][memory]") {
  CheckEveryClusterIsAnsweredFromOneWindow<Cache::Strategy::NoCache>();
  CheckEveryClusterIsAnsweredFromOneWindow<Cache::Strategy::FullCache>();
}
