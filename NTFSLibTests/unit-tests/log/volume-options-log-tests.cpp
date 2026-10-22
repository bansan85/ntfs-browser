#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/sink.h>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/log/spdlog-adapter.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

namespace NTFS_BROWSER_NAMESPACE {
namespace Io {

template <Cache::Strategy S>
class FileRecord;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

using Catch::Matchers::ContainsSubstring;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Io::FileRecord;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Tests::attr_name_exceeds_total_size_record_idx;
using NTFS_BROWSER_NAMESPACE::Tests::
    BuildFakeNtfsImageWithAttrNameExceedsTotalSize;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;

namespace {

// A salvageable condition (here, a masked-in attribute's name exceeding its
// own total_size) logs at Warn when strict and Info when recovering, with
// the same text either way.
template <Cache::Strategy S>
void RunSalvageableConditionLogLevel() {
  const auto capture_for = [&](const VolumeOptions& options) {
    std::ostringstream out;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_st>(out);
    sink->set_pattern("%l %v");
    NTFS_BROWSER_NAMESPACE::Log::SetSinks(
        {NTFS_BROWSER_NAMESPACE::Log::MakeSpdlogSink(sink)});

    const NtfsVolume<S> volume(
        std::make_unique<MemoryDiskReader>(
            BuildFakeNtfsImageWithAttrNameExceedsTotalSize()),
        options);
    REQUIRE(volume.IsVolumeOK());
    NTFS_BROWSER_NAMESPACE::Io::FileRecord<S> record(volume);
    REQUIRE(record.ParseFileRecord(attr_name_exceeds_total_size_record_idx));
    (void)record.ParseAttrs();

    NTFS_BROWSER_NAMESPACE::Tests::InstallCaptureSink();
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
