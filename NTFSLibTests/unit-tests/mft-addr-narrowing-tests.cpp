#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

TEST_CASE(
    "NtfsVolume construction must not let an exception escape when the BPB "
    "encodes an mft_addr_ too large for a LONGLONG",
    "[ntfs-volume][regression]")
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithHugeMftLcn());

  std::optional<NtfsVolume<Strategy::NO_CACHE>> volume;
  REQUIRE_NOTHROW(volume.emplace(std::move(reader)));

  REQUIRE(volume.has_value());
  CHECK_FALSE(volume->IsVolumeOK());
}
