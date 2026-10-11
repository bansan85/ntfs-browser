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
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Cache = NtfsBrowser::Cache;

namespace {

static_assert(NtfsBrowserTests::fragmented_mft_invalid_record_idx ==
                  static_cast<ULONGLONG>(NtfsBrowser::Mft::Idx::User),
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
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFragmentedMftInvalidRecord());

  const NtfsVolume<S> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);

  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      NtfsBrowserTests::fragmented_mft_invalid_record_idx));
  CHECK_FALSE(parsed);
}
