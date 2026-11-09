#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>

#include "attr/mask.h"
#include "attr/type.h"
#include "cache/strategy.h"
#include "io/file-record.h"
#include "mft/idx.h"
#include "ntfs-volume.h"
#include "win-types.h"

namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::ClearAttrs() drops the parsed attributes and keeps the header",
    "[file-record][clear-attrs]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(
          TestFakeImage::BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);
  record.SetAttrMask(Attr::Mask::FileName | Attr::Mask::Data);
  REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_report_idx));
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
  CHECK(record.GetFileReference() == TestFakeImage::mft_tree_report_idx);

  // The attributes parse again from the header that stayed.
  REQUIRE(record.ParseAttrs());
  CHECK_FALSE(record.GetAttr(Attr::Type::FileName).empty());
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::ClearAttrs() on a record with nothing "
    "parsed does nothing",
    "[file-record][clear-attrs]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(
          TestFakeImage::BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);

  record.ClearAttrs();
  CHECK(record.GetAttr(Attr::Type::FileName).empty());

  REQUIRE(record.ParseFileRecord(TestFakeImage::mft_tree_report_idx));
  record.ClearAttrs();
  record.ClearAttrs();
  CHECK(record.GetSequenceNumber() != 0);
}
