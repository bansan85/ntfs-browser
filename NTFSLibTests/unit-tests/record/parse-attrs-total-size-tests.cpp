#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "attr/type.h"
#include "cache/strategy.h"
#include "io/file-record.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "win-types.h"

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

TEMPLATE_TEST_CASE_SIG(
    "ParseAttrs rejects a resident attribute whose total_size is smaller "
    "than its header",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<
      NTFS_BROWSER_NAMESPACE::Test::DiskReaders::MemoryDiskReader>(
      NTFS_BROWSER_NAMESPACE::Test::FakeImage::
          BuildFakeNtfsImageWithUndersizedAttribute());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Test::FakeImage::undersized_attr_record_idx));

  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.GetAttr(Attr::Type::ReparsePoint).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "ParseAttrs rejects a record whose offset_of_attr exceeds its own file "
    "record size",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<
      NTFS_BROWSER_NAMESPACE::Test::DiskReaders::MemoryDiskReader>(
      NTFS_BROWSER_NAMESPACE::Test::FakeImage::
          BuildFakeNtfsImageWithAttrOffsetOutOfBounds());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));

  CHECK_FALSE(record.ParseAttrs());
}
