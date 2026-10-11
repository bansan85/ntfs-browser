#include <ntfs-browser/win-types.h>

#include <memory>
#include <sstream>
#include <string>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/log.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"

using Catch::Matchers::ContainsSubstring;
using NtfsBrowser::NtfsVolume;
using NtfsBrowser::Io::FileRecord;
namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::VolumeOptions;
using NtfsBrowserTests::attr_name_exceeds_total_size_record_idx;
using NtfsBrowserTests::BuildFakeNtfsImageWithAttrNameExceedsTotalSize;
using NtfsBrowserTests::MemoryDiskReader;

namespace {

// A salvageable condition (here, a masked-in attribute's name exceeding its
// own total_size) logs at Warn when strict and Info when recovering, with
// the same text either way.
template <Cache::Strategy S>
void RunSalvageableConditionLogLevel() {
  const std::shared_ptr<spdlog::logger> logger =
      spdlog::get(std::string(NtfsBrowser::Log::logger_name));
  REQUIRE(logger);

  const auto capture_for = [&](const VolumeOptions& options) {
    std::ostringstream out;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(out);
    sink->set_pattern("%l %v");
    logger->sinks().push_back(sink);

    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(
            BuildFakeNtfsImageWithAttrNameExceedsTotalSize()),
        options);
    REQUIRE(volume.IsVolumeOK());
    NtfsBrowser::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(attr_name_exceeds_total_size_record_idx));
    (void)record.ParseAttrs();

    logger->sinks().pop_back();
    return out.str();
  };

  const std::string strict = capture_for({});
  CHECK_THAT(strict,
             ContainsSubstring("warning Attribute name exceeds attribute "
                               "bounds."));

  const std::string recovering =
      capture_for(VolumeOptions{.recover_errors = true});
  CHECK_THAT(recovering,
             ContainsSubstring("info Attribute name exceeds attribute "
                               "bounds."));
  CHECK_THAT(recovering,
             !ContainsSubstring("warning Attribute name exceeds attribute "
                                "bounds."));
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A salvageable condition logs Warn when strict and Info when recovering",
    "[volume-options][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  RunSalvageableConditionLogLevel<S>();
}
