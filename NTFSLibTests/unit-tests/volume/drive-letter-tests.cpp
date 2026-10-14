#include <ntfs-browser/win-types.h>

#ifdef _WIN32

  #include <catch2/catch_test_macros.hpp>
  #include <catch2/matchers/catch_matchers_string.hpp>
  #include <tchar.h>

  #include <ntfs-browser/cache/strategy.h>
  #include <ntfs-browser/ntfs-volume.h>

  #include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("a drive argument that is not a letter opens no volume", "[volume]") {
  static_cast<void>(NtfsBrowserTests::TakeCapturedLog());

  const NtfsBrowser::NtfsVolume<NtfsBrowser::Cache::Strategy::NoCache> volume(
      _T('1'));

  CHECK_FALSE(volume.IsVolumeOK());
  CHECK_THAT(NtfsBrowserTests::TakeCapturedLog(),
             ContainsSubstring("Volume name error"));
}

#endif
