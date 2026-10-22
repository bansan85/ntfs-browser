#include <ntfs-browser/win-types.h>

#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/catch_message.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// Text of the warning logged when $Volume has the dirty bit set.
constexpr const char* dirty_message = "was not unmounted cleanly";

// Message the version check logs, which the dirty warning MUST follow.
constexpr const char* version_message = "NTFS volume version";

// The dirty bit of $VOLUME_INFORMATION's flags.
constexpr WORD volume_dirty = 0x0001;

// What opening an image logged, and whether the volume came up.
struct Opened {
  bool ok{false};
  std::string log;
};

template <Cache::Strategy S>
Opened Open(WORD flags, NTFS_BROWSER_NAMESPACE::VolumeOptions options) {
  (void)NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog();
  const NTFS_BROWSER_NAMESPACE::NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(
          NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithVolumeFlags(
              flags)),
      options);
  return {.ok = volume.IsVolumeOK(),
          .log = NTFS_BROWSER_NAMESPACE::Tests::TakeCapturedLog()};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG("a dirty volume is opened with a warning", "[volume]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  for (const bool recover : {false, true}) {
    INFO("recover_errors = " << recover);
    const Opened opened = Open<S>(volume_dirty, {.recover_errors = recover});

    CHECK(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(dirty_message));
    const auto version = opened.log.find(version_message);
    const auto dirty = opened.log.find(dirty_message);
    REQUIRE(version != std::string::npos);
    REQUIRE(dirty != std::string::npos);
    CHECK(version < dirty);
  }
}

TEMPLATE_TEST_CASE_SIG("a clean volume logs no dirty warning", "[volume]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  for (const bool recover : {false, true}) {
    INFO("recover_errors = " << recover);
    const Opened opened = Open<S>(0, {.recover_errors = recover});

    CHECK(opened.ok);
    CHECK_THAT(opened.log, ContainsSubstring(version_message));
    CHECK_THAT(opened.log, !ContainsSubstring(dirty_message));
  }
}

TEMPLATE_TEST_CASE_SIG("only the dirty bit triggers the warning", "[volume]",
                       ((Cache::Strategy S), S), Cache::Strategy::NoCache,
                       Cache::Strategy::FullCache) {
  for (const bool recover : {false, true}) {
    INFO("recover_errors = " << recover);
    const NTFS_BROWSER_NAMESPACE::VolumeOptions options{.recover_errors =
                                                            recover};

    for (const WORD flags : {WORD{0x0080}, WORD{0x8000}, WORD{0xFFFE}}) {
      INFO("flags = " << flags);
      const Opened opened = Open<S>(flags, options);

      CHECK(opened.ok);
      CHECK_THAT(opened.log, !ContainsSubstring(dirty_message));
    }

    for (const WORD flags : {WORD{0x0081}, WORD{0x8001}, WORD{0xFFFF}}) {
      INFO("flags = " << flags);
      const Opened opened = Open<S>(flags, options);

      CHECK(opened.ok);
      CHECK_THAT(opened.log, ContainsSubstring(dirty_message));
    }
  }
}
