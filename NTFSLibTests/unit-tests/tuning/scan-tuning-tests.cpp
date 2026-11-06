#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/knobs.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "mft/tree-testing.h"
#include "scan-comparison.h"
#include "test-log-sink.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Kind = Tests::CountingDiskReader::Kind;

namespace {

// Slots of the fixture: 3000 hold about 200 files with a list in a cluster of
// its own, so the second pass reads, announces and restarts.
constexpr ULONGLONG list_record_count = 3000;
constexpr ULONGLONG list_fragment_clusters = 250;

// The caps the library had before they were tuned.
constexpr size_t old_held_record_bytes = size_t{1} << 30U;
constexpr size_t old_restart_interval = 1024;
constexpr size_t old_waiting_batch_records = 4096;
constexpr size_t old_pending_ranges = 262144;

VolumeOptions ScanOptions() {
  return VolumeOptions{.include_deleted = true, .recover_errors = true};
}

std::vector<BYTE> ListImage() {
  return Tests::BuildFakeNtfsImageWithListMftScan(list_record_count,
                                                  list_fragment_clusters);
}

Tests::ScanResult Scan(const std::vector<BYTE>& image,
                       const VolumeOptions& options, unsigned threads) {
  static_cast<void>(Tests::TakeCapturedLog());
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(image));
  const Tests::CountingDiskReader* const counting = reader.get();
  const NtfsVolume<Cache::Strategy::NoCache> volume(std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());
  return Tests::CaptureScan(volume, counting, threads);
}

size_t Cancels(const Tests::ScanResult& scan) {
  return static_cast<size_t>(std::ranges::count_if(
      scan.calls, [](const Tests::CountingDiskReader::Call& call) {
        return call.kind == Kind::CancelPrefetch;
      }));
}

size_t Reads(const Tests::ScanResult& scan) {
  return static_cast<size_t>(std::ranges::count_if(
      scan.calls, [](const Tests::CountingDiskReader::Call& call) {
        return call.kind == Kind::Read;
      }));
}

// Restores the test hooks of the scan when it goes out of scope.
class HooksGuard final {
 public:
  HooksGuard() = default;
  HooksGuard(const HooksGuard&) = delete;
  HooksGuard& operator=(const HooksGuard&) = delete;
  HooksGuard(HooksGuard&&) = delete;
  HooksGuard& operator=(HooksGuard&&) = delete;

  ~HooksGuard() {
    Mft::SetMaxPendingRangesForTests(0);
    Mft::SetMaxHeldRecordBytesForTests(std::nullopt);
    Mft::SetWaitingBatchRecordsForTests(std::nullopt);
    Mft::SetAnnounceRestartIntervalForTests(std::nullopt);
    Mft::SetAutoThreadLimitsForTests(std::nullopt, std::nullopt);
  }
};

}  // namespace

TEST_CASE("The scan caps of the tests are the old constants",
          "[tuning][invariance][tree]") {
  const HooksGuard hooks;

  CHECK(Mft::MaxPendingRanges() == old_pending_ranges);
  CHECK(Mft::MaxHeldRecordBytes() ==
        (sizeof(void*) >= 8 ? old_held_record_bytes : size_t{128} << 20U));
  Mft::SetMaxPendingRangesForTests(7);
  Mft::SetMaxHeldRecordBytesForTests(11);
  CHECK(Mft::MaxPendingRanges() == 7);
  CHECK(Mft::MaxHeldRecordBytes() == 11);
}

TEST_CASE(
    "A scan with the default tuning reads as one with the old constants set "
    "by the hooks",
    "[tuning][invariance][tree]") {
  const HooksGuard hooks;
  const unsigned threads = GENERATE(1U, 3U);
  const std::vector<BYTE> image = ListImage();
  INFO("threads " << threads);

  const Tests::ScanResult plain = Scan(image, ScanOptions(), threads);
  CHECK(Cancels(plain) > 0);
  CHECK(Reads(plain) > 0);

  Mft::SetMaxPendingRangesForTests(old_pending_ranges);
  Mft::SetMaxHeldRecordBytesForTests(old_held_record_bytes);
  Mft::SetWaitingBatchRecordsForTests(old_waiting_batch_records);
  Mft::SetAnnounceRestartIntervalForTests(old_restart_interval);
  const Tests::ScanResult hooked = Scan(image, ScanOptions(), threads);

  Tests::CheckSameScan(plain, hooked);
}

TEST_CASE("A scan with every knob set to its default is the default scan",
          "[tuning][invariance][tree]") {
  const unsigned threads = GENERATE(1U, 3U);
  const std::vector<BYTE> image = ListImage();
  VolumeOptions explicit_defaults = ScanOptions();
  for (const Tuning::Knob& knob : Tuning::Knobs()) {
    if (knob.per_pattern) {
      for (size_t i = 0; i < Tuning::pattern_count; i++) {
        knob.set(explicit_defaults, static_cast<Tuning::Pattern>(i),
                 knob.default_value);
      }
    } else {
      knob.set(explicit_defaults, Tuning::Pattern::LargeSequential,
               knob.default_value);
    }
  }
  INFO("threads " << threads);

  Tests::CheckSameScan(Scan(image, ScanOptions(), threads),
                       Scan(image, explicit_defaults, threads));
}

TEST_CASE("The announce restart hook wins over the tuning",
          "[tuning][invariance][tree]") {
  const HooksGuard hooks;
  const std::vector<BYTE> image = ListImage();
  VolumeOptions tuned = ScanOptions();
  tuned.tuning.mft.announce_restart_interval = 40;

  const Tests::ScanResult baseline = Scan(image, ScanOptions(), 1);
  // The tuning alone restarts more often.
  const Tests::ScanResult by_tuning = Scan(image, tuned, 1);
  CHECK(Cancels(by_tuning) > Cancels(baseline));

  Mft::SetAnnounceRestartIntervalForTests(old_restart_interval);
  const Tests::ScanResult by_hook = Scan(image, tuned, 1);
  Tests::CheckSameScan(baseline, by_hook);
}

TEST_CASE("The held record hook wins over the tuning",
          "[tuning][invariance][tree]") {
  const HooksGuard hooks;
  const std::vector<BYTE> image = ListImage();
  VolumeOptions tuned = ScanOptions();
  tuned.tuning.mft.max_held_record_bytes = 0;

  const Tests::ScanResult baseline = Scan(image, ScanOptions(), 1);
  // Holding nothing makes the second pass read the records again.
  const Tests::ScanResult by_tuning = Scan(image, tuned, 1);
  CHECK(Reads(by_tuning) > Reads(baseline));

  Mft::SetMaxHeldRecordBytesForTests(old_held_record_bytes);
  const Tests::ScanResult by_hook = Scan(image, tuned, 1);
  Tests::CheckSameScan(baseline, by_hook);
}

TEST_CASE("The thread limit hooks win over the tuning of an automatic scan",
          "[tuning][invariance][tree]") {
  const HooksGuard hooks;
  const std::vector<BYTE> image = ListImage();
  // The tuning keeps an automatic scan on one thread, whatever the volume.
  VolumeOptions tuned = ScanOptions();
  tuned.tuning.mft.min_auto_threaded_slots = ~ULONGLONG{0};
  tuned.tuning.mft.probe_slots = 1000;
  tuned.tuning.mft.min_auto_threaded_waiting = 1000000;

  // The probe says no: the first pass parses on this thread alone.
  Mft::SetAutoThreadLimitsForTests(1000, 1000, false, 100);
  const Tests::ScanResult by_hook = Scan(image, tuned, 0);
  const Tests::ScanResult hooked_default = Scan(image, ScanOptions(), 0);
  CHECK(by_hook.waiting_by_threads > 0);
  CHECK(by_hook.waiting_by_threads == hooked_default.waiting_by_threads);
  Tests::CheckSameScan(hooked_default, by_hook);

  // Without the hook for the waiting count, the tuning is read.
  Mft::SetAutoThreadLimitsForTests(1000, 1000, false);
  CHECK(Scan(image, tuned, 0).waiting_by_threads == 0);
  tuned.tuning.mft.min_auto_threaded_waiting = 100;
  CHECK(Scan(image, tuned, 0).waiting_by_threads > 0);
}
