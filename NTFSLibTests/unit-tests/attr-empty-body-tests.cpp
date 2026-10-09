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

namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;

TEMPLATE_TEST_CASE_SIG(
    "ParseAttrs rejects a resident STANDARD_INFORMATION with an empty body",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithEmptyStandardInformation());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);
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
