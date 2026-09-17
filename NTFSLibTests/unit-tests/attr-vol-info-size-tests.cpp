#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

TEST_CASE(
    "NtfsVolume must accept a real-size (12-byte) VOLUME_INFORMATION "
    "attribute, not just whatever sizeof(Attr::VolumeInformation) currently "
    "computes to",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithMinimalVolumeInformation());

  NtfsVolume<Strategy::NO_CACHE> volume(std::move(reader));

  CHECK(volume.IsVolumeOK());
  CHECK(volume.GetVersion() == std::pair<BYTE, BYTE>{3, 1});
}
