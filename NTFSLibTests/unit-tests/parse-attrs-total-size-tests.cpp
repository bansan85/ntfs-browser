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
    "ParseAttrs rejects a resident attribute whose total_size is smaller "
    "than its header",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUndersizedAttribute());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kUndersizedAttrRecordIdx));

  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::REPARSE_POINT).empty());
}

TEST_CASE(
    "ParseAttrs rejects a resident attribute whose total_size is smaller "
    "than its header (FULL_CACHE)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithUndersizedAttribute());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);
  REQUIRE(record.ParseFileRecord(NtfsBrowserTests::kUndersizedAttrRecordIdx));

  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::REPARSE_POINT).empty());
}
