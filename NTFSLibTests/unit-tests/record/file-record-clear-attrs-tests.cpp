#include <memory>
#include <utility>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::ClearAttrs() drops the parsed attributes and keeps the header",
    "[file-record][clear-attrs]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(
      Tests::BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);
  record.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
  REQUIRE(record.ParseFileRecord(Tests::mft_tree_report_idx));
  REQUIRE(record.ParseAttrs());
  REQUIRE_FALSE(record.GetAttr(Attr::Type::FileName).empty());
  REQUIRE_FALSE(record.GetAttr(Attr::Type::StandardInformation).empty());
  const WORD sequence = record.GetSequenceNumber();

  record.ClearAttrs();

  CHECK(record.GetAttr(Attr::Type::FileName).empty());
  CHECK(record.GetAttr(Attr::Type::StandardInformation).empty());
  CHECK(record.FindStream({}) == nullptr);
  CHECK(record.GetSequenceNumber() == sequence);
  CHECK_FALSE(record.IsDeleted());
  CHECK(record.GetFileReference() == Tests::mft_tree_report_idx);

  // The attributes parse again from the header that stayed.
  REQUIRE(record.ParseAttrs());
  CHECK_FALSE(record.GetAttr(Attr::Type::FileName).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::ClearAttrs() on a record with nothing "
    "parsed does nothing",
    "[file-record][clear-attrs]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(
      Tests::BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);

  record.ClearAttrs();
  CHECK(record.GetAttr(Attr::Type::FileName).empty());

  REQUIRE(record.ParseFileRecord(Tests::mft_tree_report_idx));
  record.ClearAttrs();
  record.ClearAttrs();
  CHECK(record.GetSequenceNumber() != 0);
}
