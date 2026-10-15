#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/base.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

TEMPLATE_TEST_CASE_SIG(
    "GetAttrName rejects a name whose offset/length exceed the attribute's "
    "total_size, when recovering",
    "[attr-base][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithAttrNameExceedsTotalSize());

  const NtfsVolume<S> volume(std::move(reader),
                             VolumeOptions{.recover_errors = true});
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::attr_name_exceeds_total_size_record_idx));
  REQUIRE(record.ParseAttrs());

  const auto& data_attrs = record.GetAttr(Attr::Type::Data);
  REQUIRE(data_attrs.size() == 1);

  // The REQUIRE above checks the size of dataAttrs.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(data_attrs[0]->GetAttrName().empty());
}

TEMPLATE_TEST_CASE_SIG(
    "A masked-in attribute name exceeding total_size rejects the whole "
    "record by default",
    "[attr-base][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithAttrNameExceedsTotalSize());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(
      NTFS_BROWSER_NAMESPACE::Tests::attr_name_exceeds_total_size_record_idx));
  CHECK_FALSE(record.ParseAttrs());
  CHECK(record.GetAttr(Attr::Type::Data).empty());
}
