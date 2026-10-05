#include <ntfs-browser/win-types.h>

#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/file-record.h>
#include <ntfs-browser/mft-idx.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileRecord;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

namespace
{
static_assert(NtfsBrowserTests::fragmented_mft_invalid_record_idx ==
                  static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::User),
              "this fixture's whole point is to be reached through "
              "FileRecord<S>::ReadFileRecord()'s \"fragmented $MFT\" branch "
              "(fileRef >= Enum::MftIdx::USER), not the direct-allocation "
              "one - see fake-ntfs-image.h");
}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::ParseFileRecord() must not let an exception escape on the "
    "fragmented-$MFT path when the forged record has an invalid "
    "offset_of_us",
    "[file-record][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFragmentedMftInvalidRecord());

  NtfsVolume<S> const volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<S> record(volume);

  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      NtfsBrowserTests::fragmented_mft_invalid_record_idx));
  CHECK_FALSE(parsed);
}
