#include <memory>

#include <catch2/catch_test_macros.hpp>

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
static_assert(NtfsBrowserTests::kFragmentedMftInvalidRecordIdx ==
                  static_cast<ULONGLONG>(NtfsBrowser::Enum::MftIdx::USER),
              "this fixture's whole point is to be reached through "
              "FileRecord<S>::ReadFileRecord()'s \"fragmented $MFT\" branch "
              "(fileRef >= Enum::MftIdx::USER), not the direct-allocation "
              "one - see fake-ntfs-image.h");
}  // namespace

TEST_CASE(
    "FileRecord::ParseFileRecord() must not let an exception escape on the "
    "fragmented-$MFT path when the forged record has an invalid "
    "offset_of_us",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFragmentedMftInvalidRecord());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::NO_CACHE> record(volume);

  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      NtfsBrowserTests::kFragmentedMftInvalidRecordIdx));
  CHECK_FALSE(parsed);
}

TEST_CASE(
    "FileRecord::ParseFileRecord() must not let an exception escape on the "
    "fragmented-$MFT path when the forged record has an invalid "
    "offset_of_us (FULL_CACHE)",
    "[file-record][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFragmentedMftInvalidRecord());

  NtfsVolume<Strategy::FULL_CACHE> volume(std::move(reader));
  REQUIRE(volume.IsVolumeOK());

  FileRecord<Strategy::FULL_CACHE> record(volume);

  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(
                      NtfsBrowserTests::kFragmentedMftInvalidRecordIdx));
  CHECK_FALSE(parsed);
}
