#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/ntfs-volume.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "test-log-sink.h"

using NtfsBrowser::NtfsVolume;
namespace Cache = NtfsBrowser::Cache;

TEMPLATE_TEST_CASE_SIG(
    "NtfsVolume construction must not let an exception escape when the BPB "
    "encodes an mft_addr_ too large for a LONGLONG",
    "[ntfs-volume][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  auto reader = std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
      NtfsBrowserTests::BuildFakeNtfsImageWithHugeMftLcn());

  std::optional<NtfsVolume<S>> volume;
  REQUIRE_NOTHROW(volume.emplace(std::move(reader)));

  REQUIRE(volume.has_value());
  CHECK_FALSE(volume->IsVolumeOK());
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecord::ParseFileRecord() must not let an exception escape when the "
    "$MFT address overflows",
    "[file-record][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const NtfsVolume<S> volume(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(
          NtfsBrowserTests::BuildFakeNtfsImageWithHugeMftLcn()));
  REQUIRE_FALSE(volume.IsVolumeOK());

  NtfsBrowser::Io::FileRecord<S> record(volume);
  (void)NtfsBrowserTests::TakeCapturedLog();
  bool parsed = true;
  REQUIRE_NOTHROW(parsed = record.ParseFileRecord(0));
  CHECK_FALSE(parsed);
  CHECK(NtfsBrowserTests::TakeCapturedLog().find("Cannot read file record 0") !=
        std::string::npos);
}
