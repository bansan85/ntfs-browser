#include <ntfs-browser/win-types.h>

#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
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

TEMPLATE_TEST_CASE_SIG(
    "ParseAttrs rejects a resident STANDARD_INFORMATION with an empty body",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithEmptyStandardInformation());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::legacy_standard_information_record_idx));

  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.GetAttr(Attr::Type::StandardInformation).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a resident VOLUME_INFORMATION with an empty body",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithEmptyVolumeInformation());

  const NtfsVolume<S> volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "ParseAttrs accepts a real-size (48-byte) NTFS 1.2 STANDARD_INFORMATION "
    "attribute, not just whatever sizeof(Data::StandardInformation) "
    "currently computes to",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithLegacyStandardInformation());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::legacy_standard_information_record_idx));

  CHECK(record.ParseAttrs());
  CHECK_FALSE(record.GetAttr(Attr::Type::StandardInformation).empty());
  CHECK(record.IsReadOnly());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must accept a real-size (12-byte) VOLUME_INFORMATION "
    "attribute, not just whatever sizeof(Data::VolumeInformation) currently "
    "computes to",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithMinimalVolumeInformation());

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
  CHECK(volume.GetVersion() == std::pair<BYTE, BYTE>{3, 1});
}
