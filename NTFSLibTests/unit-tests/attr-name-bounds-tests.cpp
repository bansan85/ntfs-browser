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
    "[attr-base][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttrNameExceedsTotalSize());

  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::attr_name_exceeds_total_size_record_idx));
  REQUIRE(record.ParseAttrs());

  const auto& data_attrs = record.GetAttr(AttrType::Data);
  REQUIRE(data_attrs.size() == 1);

  // The REQUIRE above checks the size of dataAttrs.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(data_attrs[0]->GetAttrName().empty());
}

TEMPLATE_TEST_CASE_SIG(
    "A masked-in attribute name exceeding total_size rejects the whole "
    "record by default",
    "[attr-base][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithAttrNameExceedsTotalSize());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::attr_name_exceeds_total_size_record_idx));
  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.GetAttr(AttrType::Data).empty());
}
