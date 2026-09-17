#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

TEST_CASE(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far larger than any plausible size",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedIndexBlock());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK_FALSE(volume.IsVolumeOK());
}

TEST_CASE(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "far larger than any plausible size",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithOversizedFileRecord());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  INFO("GetFileRecordSize() = " << volume.GetFileRecordSize());
  // GetFileRecordSize() reflects the BPB value directly; IsVolumeOK() fails
  // regardless, from an unrelated $Volume read failure this size triggers.
  CHECK(volume.GetFileRecordSize() !=
        NtfsBrowserTests::kOversizedFileRecordSize);
}

TEST_CASE(
    "NtfsVolume must not accept a volume whose BPB describes a file record "
    "size that exceeds kMaxFileRecordSize via the positive "
    "clusters_per_file_record branch",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithFileRecordSizeTooBig());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  CHECK(volume.GetFileRecordSize() == NtfsBrowserTests::kFileRecordSizeTooBig);
  CHECK_FALSE(volume.IsVolumeOK());
}
