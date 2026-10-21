#include <ntfs-browser/win-types.h>

#include <memory>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/attr/mask.h>
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
    "ParseFileRecord refuses record bytes that are not one record long",
    "[file-record]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(
      Tests::BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);

  SECTION("too short") {
    const std::vector<BYTE> bytes(Tests::fake_file_record_size - 1, 0);
    CHECK_FALSE(record.ParseFileRecord(Tests::mft_tree_docs_idx, bytes));
  }
  SECTION("too long") {
    const std::vector<BYTE> bytes(Tests::fake_file_record_size + 1, 0);
    CHECK_FALSE(record.ParseFileRecord(Tests::mft_tree_docs_idx, bytes));
  }
}

TEMPLATE_TEST_CASE_SIG("a record with no unnamed $DATA has no allocated size",
                       "[file-record]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(
      Tests::BuildFakeNtfsImageWithMftTree()));
  REQUIRE(volume.IsVolumeOK());
  FileRecord<S> record(volume);
  REQUIRE(record.ParseFileRecord(Tests::mft_tree_docs_idx));
  REQUIRE(record.ParseAttrs());

  CHECK(record.FindStream({}) == nullptr);
  CHECK(record.GetAllocatedSize() == 0);
}
