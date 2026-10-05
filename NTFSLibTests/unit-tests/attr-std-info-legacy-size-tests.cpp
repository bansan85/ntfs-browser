#include <memory>
#include <utility>
#include <vector>

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
    "ParseAttrs accepts a real-size (48-byte) NTFS 1.2 STANDARD_INFORMATION "
    "attribute, not just whatever sizeof(Attr::StandardInformation) "
    "currently computes to",
    "[file-record][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithLegacyStandardInformation());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NtfsBrowserTests::legacy_standard_information_record_idx));

  CHECK(record.ParseAttrs());
  CHECK_FALSE(record.GetAttr(AttrType::StandardInformation).empty());
  CHECK(record.IsReadOnly());
}
