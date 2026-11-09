#ifdef _WIN32

  #include <catch2/catch_test_macros.hpp>
  #include <catch2/matchers/catch_matchers_string.hpp>
  #include <tchar.h>

  #include <ntfs-browser/test/support/test-log-sink.h>

  #include "cache/strategy.h"
  #include "ntfs-volume.h"

using Catch::Matchers::ContainsSubstring;

TEST_CASE("a drive argument that is not a letter opens no volume", "[volume]") {
  static_cast<void>(NTFS_BROWSER_NAMESPACE::Test::Support::TakeCapturedLog());

  const NTFS_BROWSER_NAMESPACE::NtfsVolume<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>
      volume(_T('1'));

  CHECK_FALSE(volume.IsVolumeOK());
  CHECK_THAT(NTFS_BROWSER_NAMESPACE::Test::Support::TakeCapturedLog(),
             ContainsSubstring("Volume name error"));
}

#endif
