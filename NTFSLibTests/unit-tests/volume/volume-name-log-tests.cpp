#include <algorithm>
#include <memory>
#include <string>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/ntfs-volume.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;

TEMPLATE_TEST_CASE_SIG("the volume name is logged without its terminator",
                       "[logging]", ((NtfsBrowser::Cache::Strategy S), S),
                       NtfsBrowser::Cache::Strategy::NoCache,
                       NtfsBrowser::Cache::Strategy::FullCache) {
  (void)NtfsBrowserTests::TakeCapturedLog();
  const NtfsBrowser::NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithVolumeName()));
  const std::string captured = NtfsBrowserTests::TakeCapturedLog();

  CHECK(volume.IsVolumeOK());
  CHECK_THAT(captured, ContainsSubstring("NTFS volume name: TESTVOL"));
  // AttrVolName pads its buffer with a terminator its view still covers.
  // UTF-8 has no terminator, so a NUL byte must not reach the line.
  CHECK(std::ranges::find(captured, '\0') == captured.end());
}
