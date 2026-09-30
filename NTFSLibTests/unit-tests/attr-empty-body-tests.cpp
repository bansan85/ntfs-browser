#include <memory>
#include <utility>

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

TEST_CASE(
    "ParseAttrs rejects a resident STANDARD_INFORMATION with an empty body "
    "(FULL_CACHE)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEmptyStandardInformation());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::kLegacyStandardInformationRecordIdx));

  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::STANDARD_INFORMATION).empty());
}

TEST_CASE(
    "NtfsVolume rejects a resident VOLUME_INFORMATION with an empty body "
    "(FULL_CACHE)",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEmptyVolumeInformation());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());
}
