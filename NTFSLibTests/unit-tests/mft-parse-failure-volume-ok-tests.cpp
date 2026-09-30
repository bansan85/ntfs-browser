#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/strategy.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Strategy;

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not report IsVolumeOK() == true, nor let "
    "GetRecordsCount() dereference a null $MFT DATA attribute, when $MFT's "
    "own file record fails to parse",
    "[ntfs-volume][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithCorruptMftRecord());

  NtfsVolume<S> volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());

  // A crash here takes down only this test's own, isolated ctest process.
  CHECK(volume.GetRecordsCount() == 0);
}
