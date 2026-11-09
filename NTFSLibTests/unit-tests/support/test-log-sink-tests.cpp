#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "log/ntfs-common.h"

namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;

TEST_CASE("A test starts with an empty captured log", "[support][log]") {
  TestSupport::InstallCaptureSink();
  Log::Warn("left behind by the test before");

  TestSupport::InstallCaptureSink();

  CHECK(TestSupport::TakeCapturedLog().empty());
}

TEST_CASE("The captured log keeps what a test logs after the install",
          "[support][log]") {
  TestSupport::InstallCaptureSink();

  Log::Warn("kept {}", 7);

  CHECK(TestSupport::TakeCapturedLog() == "kept 7\n");
  CHECK(TestSupport::TakeCapturedLog().empty());
}
