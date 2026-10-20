#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "alloc-test-support.h"
#include "alloc/cluster-bitmap.h"
#include "fake-ntfs-image.h"

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
  const auto opened = Support::Open<S>(image);

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

}  // namespace

TEST_CASE("a bitmap of several windows is never held whole",
          "[alloc][memory]") {
  CheckEveryClusterIsAnsweredFromOneWindow<Cache::Strategy::NoCache>();
  CheckEveryClusterIsAnsweredFromOneWindow<Cache::Strategy::FullCache>();
}
