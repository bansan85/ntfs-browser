#include <memory>
#include <utility>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

TEMPLATE_TEST_CASE_SIG(
    "ParseAttrs rejects a resident STANDARD_INFORMATION with an empty body",
    "[file-record][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEmptyStandardInformation());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::kLegacyStandardInformationRecordIdx));

  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::STANDARD_INFORMATION).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume rejects a resident VOLUME_INFORMATION with an empty body",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEmptyVolumeInformation());

  NtfsVolume<S> const volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());
}
