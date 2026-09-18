#include <memory>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/data/attr-type.h>
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
    "ParseAttrs accepts a real-size (48-byte) NTFS 1.2 STANDARD_INFORMATION "
    "attribute, not just whatever sizeof(Attr::StandardInformation) "
    "currently computes to",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithLegacyStandardInformation());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::kLegacyStandardInformationRecordIdx));

  CHECK(record.ParseAttrs());
  CHECK_FALSE(record.getAttr(AttrType::STANDARD_INFORMATION).empty());
  CHECK(record.IsReadOnly());
}

TEST_CASE(
    "ParseAttrs accepts a real-size (48-byte) NTFS 1.2 STANDARD_INFORMATION "
    "attribute, not just whatever sizeof(Attr::StandardInformation) "
    "currently computes to (FULL_CACHE)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithLegacyStandardInformation());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::kLegacyStandardInformationRecordIdx));

  CHECK(record.ParseAttrs());
  CHECK_FALSE(record.getAttr(AttrType::STANDARD_INFORMATION).empty());
  CHECK(record.IsReadOnly());
}
