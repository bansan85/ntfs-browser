#include <memory>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "volume-open.h"

using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Cache::Strategy;
using NtfsBrowserTests::BuildFakeNtfsImageWithMftTree;
using NtfsBrowserTests::MemoryDiskReader;
using NtfsCompare::comparison_volume_options;

TEST_CASE("The comparison volume options are the library defaults",
          "[ntfscompare][volume-open]") {
  CHECK_FALSE(comparison_volume_options.include_deleted);
  CHECK_FALSE(comparison_volume_options.recover_errors);
}

TEMPLATE_TEST_CASE_SIG(
    "A volume built with the comparison options reports them back",
    "[ntfscompare][volume-open]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithMftTree()),
      comparison_volume_options);
  REQUIRE(volume.IsVolumeOK());

  CHECK_FALSE(volume.GetOptions().include_deleted);
  CHECK_FALSE(volume.GetOptions().recover_errors);
}
