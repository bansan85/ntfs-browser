#include <memory>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume must not report IsVolumeOK() == true, nor let "
    "GetRecordsCount() dereference a null $MFT DATA attribute, when $MFT's "
    "own file record fails to parse",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<
      NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithCorruptMftRecord());

  const NtfsVolume<S> volume(std::move(reader));

  CHECK_FALSE(volume.IsVolumeOK());

  // A crash here takes down only this test's own, isolated ctest process.
  CHECK(volume.GetRecordsCount() == 0);
}
