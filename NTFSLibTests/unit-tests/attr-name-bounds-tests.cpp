#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr-base.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::AttrType;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;
using NtfsBrowser::VolumeOptions;

TEMPLATE_TEST_CASE_SIG(
    "GetAttrName rejects a name whose offset/length exceed the attribute's "
    "total_size, when recovering",
    "[attr-base][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttrNameExceedsTotalSize());

  NtfsVolume<S> const volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::kAttrNameExceedsTotalSizeRecordIdx));
  REQUIRE(record.ParseAttrs());

  const auto& dataAttrs = record.getAttr(AttrType::DATA);
  REQUIRE(dataAttrs.size() == 1);

  CHECK(dataAttrs[0]->GetAttrName().empty());
}

TEMPLATE_TEST_CASE_SIG(
    "A masked-in attribute name exceeding total_size rejects the whole "
    "record by default",
    "[attr-base][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttrNameExceedsTotalSize());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::kAttrNameExceedsTotalSizeRecordIdx));
  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.getAttr(AttrType::DATA).empty());
}
