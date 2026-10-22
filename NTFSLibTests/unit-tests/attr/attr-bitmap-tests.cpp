#include <ntfs-browser/win-types.h>

#include <memory>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;

namespace {

template <Cache::Strategy S>
void CheckMultiClusterBitmapIsWrapped() {
  const NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithMultiClusterBitmap()));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(record.ParseAttrs());

  const auto& bitmap_attrs = record.GetAttr(Attr::Type::Bitmap);
  REQUIRE(bitmap_attrs.size() == 1);
  // The REQUIRE above checks the size of bitmap_attrs.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  const auto& bitmap = *bitmap_attrs[0];
  CHECK(bitmap.IsNonResident());
  CHECK(bitmap.GetDataSize() ==
        NTFS_BROWSER_NAMESPACE::Tests::multi_cluster_bitmap_clusters *
            NTFS_BROWSER_NAMESPACE::Tests::fake_cluster_size);
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("A non-resident bitmap attribute spans several clusters",
                       "[attr-bitmap][regression]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  CheckMultiClusterBitmapIsWrapped<S>();
}
