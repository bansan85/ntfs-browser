#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "ntfs-browser/win-types.h"
#include "test-log-sink.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
using Catch::Matchers::ContainsSubstring;

namespace {

// 2^54 records of 1 KiB put the byte offset at 2^64, which wraps a ULONGLONG
// to 0: this record aliases record 0 on the direct path.
constexpr ULONGLONG wrapping_record_idx = 1ULL << 54U;

// The same wrap, plus Mft::Idx::User, lands on record 16 through $MFT's data.
constexpr ULONGLONG wrapping_mapped_record_idx =
    (1ULL << 54U) + static_cast<ULONGLONG>(Mft::Idx::User);

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A record past Mft::Idx::User whose byte offset wraps a ULONGLONG is "
    "rejected, not aliased to a record read through $MFT's data",
    "[file-record][disk-read][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NTFS_BROWSER_NAMESPACE::NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImage()));
  REQUIRE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(wrapping_mapped_record_idx));
  CHECK_FALSE(parsed);
  CHECK_THAT(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog(),
             ContainsSubstring("past the end of $MFT"));
}

TEMPLATE_TEST_CASE_SIG(
    "A direct record whose byte offset wraps a ULONGLONG is rejected, not "
    "aliased to record 0",
    "[file-record][disk-read][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // A volume whose $MFT does not parse has no extent map, so every record
  // takes the direct path.
  const NTFS_BROWSER_NAMESPACE::NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::
              BuildFakeNtfsImageWithCorruptMftRecord()));
  REQUIRE_FALSE(volume.IsVolumeOK());

  NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(wrapping_record_idx));
  CHECK_FALSE(parsed);
  CHECK_THAT(NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog(),
             ContainsSubstring("past the end of $MFT"));
}
