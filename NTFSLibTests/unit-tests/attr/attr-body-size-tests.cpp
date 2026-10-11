#include <ntfs-browser/win-types.h>

#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Cache = NtfsBrowser::Cache;

TEMPLATE_TEST_CASE_SIG(
    "ParseAttrs rejects a resident STANDARD_INFORMATION with an empty body",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEmptyStandardInformation());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::legacy_standard_information_record_idx));

  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.GetAttr(Attr::Type::StandardInformation).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a resident VOLUME_INFORMATION with an empty body",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEmptyVolumeInformation());

  const NtfsVolume<S> volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "ParseAttrs accepts a real-size (48-byte) NTFS 1.2 STANDARD_INFORMATION "
    "attribute, not just whatever sizeof(Data::StandardInformation) "
    "currently computes to",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithLegacyStandardInformation());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::legacy_standard_information_record_idx));

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
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMinimalVolumeInformation());

  const NtfsVolume<S> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
  CHECK(volume.GetVersion() == std::pair<BYTE, BYTE>{3, 1});
}
