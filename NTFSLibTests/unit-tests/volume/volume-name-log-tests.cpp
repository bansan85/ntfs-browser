#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;

TEMPLATE_TEST_CASE_SIG("the volume name is logged without its terminator",
                       "[logging]",
                       ((NTFS_BROWSER_NAMESPACE::Cache::Strategy S), S),
                       NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache,
                       NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache) {
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NTFS_BROWSER_NAMESPACE::NtfsVolume<S> volume(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithVolumeName()));
  const std::string captured = NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();

  CHECK(volume.IsVolumeOK());
  CHECK_THAT(captured, ContainsSubstring("NTFS volume name: TESTVOL"));
  // AttrVolName pads its buffer with a terminator its view still covers.
  // UTF-8 has no terminator, so a NUL byte must not reach the line.
  CHECK(std::ranges::find(captured, '\0') == captured.end());
}
