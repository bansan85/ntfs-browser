#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

// Regression fixture for the bug fixed alongside F44
// (docs/bug-reports/2026-09-03-full-repo.md): AttrVolInfo's ctor
// (src/attr-vol-info.cpp) throws if GetDataSize() < sizeof(Attr::
// VolumeInformation). The real on-disk $VOLUME_INFORMATION attribute is
// exactly 12 bytes (8-byte reserved + 1-byte major + 1-byte minor + 2-byte
// flags) - but without #pragma pack(1) on the struct, alignof(ULONGLONG)
// pads sizeof(Attr::VolumeInformation) up to 16, so that check rejected
// every real volume's attribute. NtfsVolume<S>::Init() (src/ntfs-volume.cpp)
// parses $Volume's VOLUME_INFORMATION on every single volume open, so this
// silently broke opening any real NTFS volume (IsVolumeOK() stayed false)
// while every existing test kept passing, because
// BuildFakeNtfsImage()'s own $Volume fixture sizes its attribute off the
// very same (possibly inflated) sizeof(), staying self-consistent with
// the bug instead of exposing it.
//
// BuildFakeNtfsImageWithMinimalVolumeInformation() instead hardcodes the
// attribute's declared size to the true on-disk value
// (kMinimalVolumeInformationSize, 12), independent of
// sizeof(Attr::VolumeInformation), so this test keeps failing if the
// padding/size mismatch is ever reintroduced.
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
