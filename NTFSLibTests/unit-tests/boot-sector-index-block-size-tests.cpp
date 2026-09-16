#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

TEST_CASE(
    "NtfsVolume must not accept a volume whose BPB describes an index block "
    "far smaller than Data::IndexBlock",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithTinyIndexBlock());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  INFO("GetIndexBlockSize() = " << volume.GetIndexBlockSize());
  CHECK(volume.GetIndexBlockSize() == NtfsBrowserTests::kTinyIndexBlockSize);

  // kTinyIndexBlockSize doesn't fit Data::IndexBlock's own header.
  CHECK_FALSE(volume.IsVolumeOK());
}
