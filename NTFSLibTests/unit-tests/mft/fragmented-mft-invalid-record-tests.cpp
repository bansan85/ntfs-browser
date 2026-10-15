#include <ntfs-browser/win-types.h>

#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

static_assert(
    NTFS_BROWSER_NAMESPACE::Tests::fragmented_mft_invalid_record_idx ==
        static_cast<ULONGLONG>(NTFS_BROWSER_NAMESPACE::Mft::Idx::User),
    "this fixture's whole point is to be reached through "
    "FileRecord<S>::ReadFileRecord()'s \"fragmented $MFT\" branch "
    "(fileRef >= Mft::Idx::User), not the direct-allocation "
    "one - see fake-ntfs-image.h");

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::ParseFileRecord() must not let an exception escape on the "
    "fragmented-$MFT path when the forged record has an invalid "
    "offset_of_us",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithFragmentedMftInvalidRecord());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

  bool parsed = true;
  REQUIRE_NOTHROW(
      parsed = record.ParseFileRecord(
          NTFS_BROWSER_NAMESPACE::Tests::fragmented_mft_invalid_record_idx));
  CHECK_FALSE(parsed);
  CHECK(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog().find(
            "Offset must be lower than 1024.") != std::string::npos);
}
