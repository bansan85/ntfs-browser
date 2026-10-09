#include <ntfs-browser/win-types.h>

#include <memory>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr-base.h>  // IWYU pragma: keep
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>  // IWYU pragma: keep
#include <ntfs-browser/strategy.h>

#include "attr-bitmap.h"
#include "attr-non-resident.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::Attr::AttrBitmap;
using NtfsBrowser::Attr::AttrNonResident;
namespace Mft = NtfsBrowser::Mft;

namespace {

// Bitmap bits one fake cluster holds: what separates one bitmap cluster from
// the next in a cluster index.
constexpr ULONGLONG bits_per_bitmap_cluster =
    static_cast<ULONGLONG>(NtfsBrowserTests::fake_cluster_size) * 8;

template <Cache::Strategy S>
void CheckClusterFreeAnswersPastTheFirstBitmapCluster() {
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithMultiClusterBitmap()));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(record.ParseAttrs());

  const auto& bitmap_attrs = record.GetAttr(Attr::Type::Bitmap);
  REQUIRE(bitmap_attrs.size() == 1);
  auto& bitmap =
      // The REQUIRE above checks the size of bitmapAttrs.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      static_cast<AttrBitmap<AttrNonResident<S>, S>&>(*bitmap_attrs[0]);

  // Second bitmap cluster: all zeros, so every cluster it tracks is free.
  CHECK(bitmap.IsClusterFree(bits_per_bitmap_cluster));
  CHECK(bitmap.IsClusterFree(bits_per_bitmap_cluster + 7));

  // Third bitmap cluster: only its first bit is set.
  CHECK_FALSE(bitmap.IsClusterFree(2 * bits_per_bitmap_cluster));
  CHECK(bitmap.IsClusterFree(2 * bits_per_bitmap_cluster + 1));

  // First bitmap cluster: all ones, read again after leaving it.
  CHECK_FALSE(bitmap.IsClusterFree(0));
  CHECK_FALSE(bitmap.IsClusterFree(bits_per_bitmap_cluster - 1));

  // Back to the second one.
  CHECK(bitmap.IsClusterFree(bits_per_bitmap_cluster + 1));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A non-resident bitmap answers for clusters past its first cluster",
    "[attr-bitmap][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckClusterFreeAnswersPastTheFirstBitmapCluster<S>();
}
