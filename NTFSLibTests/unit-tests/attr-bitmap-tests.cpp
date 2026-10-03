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

using NtfsBrowser::AttrBitmap;
using NtfsBrowser::AttrNonResident;
using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::Enum::MftIdx;

namespace
{

// Bitmap bits one fake cluster holds: what separates one bitmap cluster from
// the next in a cluster index.
constexpr ULONGLONG kBitsPerBitmapCluster =
    static_cast<ULONGLONG>(NtfsBrowserTests::kFakeClusterSize) * 8;

template <Strategy S>
void CheckClusterFreeAnswersPastTheFirstBitmapCluster()
{
  NtfsVolume<S> const volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithMultiClusterBitmap()));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(MftIdx::ROOT)));
  REQUIRE(record.ParseAttrs());

  const auto& bitmapAttrs = record.getAttr(AttrType::BITMAP);
  REQUIRE(bitmapAttrs.size() == 1);
  auto& bitmap =
      // The REQUIRE above checks the size of bitmapAttrs.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      static_cast<AttrBitmap<AttrNonResident<S>, S>&>(*bitmapAttrs[0]);

  // Second bitmap cluster: all zeros, so every cluster it tracks is free.
  CHECK(bitmap.IsClusterFree(kBitsPerBitmapCluster));
  CHECK(bitmap.IsClusterFree(kBitsPerBitmapCluster + 7));

  // Third bitmap cluster: only its first bit is set.
  CHECK_FALSE(bitmap.IsClusterFree(2 * kBitsPerBitmapCluster));
  CHECK(bitmap.IsClusterFree(2 * kBitsPerBitmapCluster + 1));

  // First bitmap cluster: all ones, read again after leaving it.
  CHECK_FALSE(bitmap.IsClusterFree(0));
  CHECK_FALSE(bitmap.IsClusterFree(kBitsPerBitmapCluster - 1));

  // Back to the second one.
  CHECK(bitmap.IsClusterFree(kBitsPerBitmapCluster + 1));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A non-resident bitmap answers for clusters past its first cluster",
    "[attr-bitmap][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  CheckClusterFreeAnswersPastTheFirstBitmapCluster<S>();
}
