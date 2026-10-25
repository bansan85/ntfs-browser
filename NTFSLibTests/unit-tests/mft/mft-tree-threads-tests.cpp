#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/attr/defines.h>
#include <ntfs-browser/attr/header-common.h>
#include <ntfs-browser/attr/type.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "counting-disk-reader.h"
#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "mft/tree-testing.h"
#include "scan-comparison.h"
#include "test-log-sink.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using NTFS_BROWSER_NAMESPACE::Mft::MftTree;
namespace Attr = NTFS_BROWSER_NAMESPACE::Attr;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;

namespace {

// Slots of the busy fixtures: past two windows of the usual 4 MiB at 1 KiB per
// record, so a scan meets three windows and a short last one.
constexpr ULONGLONG busy_record_count = 8300;

// Clusters in each fragment of the busy fixtures: the run list of $MFT fits
// the first sector of its record up to about 4000 slots at 250, and 8300 at
// 255, the most one length byte holds.
constexpr ULONGLONG busy_fragment_clusters = 255;

using Tests::CheckSameScan;
using Tests::ScanResult;

// Scans image with threads threads, and gathers what it did. progress_stops_at,
// when not zero, makes the scan's progress callback stop it at that call.
template <Cache::Strategy S>
ScanResult ScanWithThreads(const std::vector<BYTE>& image,
                           const VolumeOptions& options, unsigned threads,
                           size_t progress_stops_at = 0) {
  static_cast<void>(Tests::TakeCapturedLog());
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(image));
  const Tests::CountingDiskReader* const counting = reader.get();
  const NtfsVolume<S> volume(std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());
  return Tests::CaptureScan(volume, counting, threads, progress_stops_at);
}

// Checks that scanning image with each thread count gives what one thread
// gives.
template <Cache::Strategy S>
void CheckSameWhateverTheThreads(const std::vector<BYTE>& image,
                                 const VolumeOptions& options,
                                 size_t progress_stops_at = 0) {
  const ScanResult one =
      ScanWithThreads<S>(image, options, 1, progress_stops_at);
  CHECK(one.parsed_by_threads == 0);
  CHECK_FALSE(one.tree.empty());
  CHECK_FALSE(one.log.empty());

  for (const unsigned threads : {2U, 3U, 8U}) {
    const ScanResult many =
        ScanWithThreads<S>(image, options, threads, progress_stops_at);
    INFO("threads " << threads);
    if (progress_stops_at != 1) {
      CHECK(many.parsed_by_threads > 0);
    }
    CheckSameScan(one, many);
  }
}

// Options of a scan: strict or recovering, with or without the freed records.
VolumeOptions OptionsOf(bool include_deleted, bool recover) {
  return VolumeOptions{.include_deleted = include_deleted,
                       .recover_errors = recover};
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "A scan over several windows gives the same tree, log and reads whatever "
    "the thread count",
    "[mft][tree][threads]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const bool include_deleted = GENERATE(false, true);
  const bool recover = GENERATE(false, true);
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithBusyMftScan(
      busy_record_count, busy_fragment_clusters);

  CheckSameWhateverTheThreads<S>(image, OptionsOf(include_deleted, recover));
}

TEMPLATE_TEST_CASE_SIG(
    "A scan of the plain fragmented fixture gives the same tree whatever the "
    "thread count",
    "[mft][tree][threads]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const bool include_deleted = GENERATE(false, true);
  const bool recover = GENERATE(false, true);
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithFragmentedMftScan(4000, 250);

  CheckSameWhateverTheThreads<S>(image, OptionsOf(include_deleted, recover));
}

TEMPLATE_TEST_CASE_SIG(
    "A scan of files with an $ATTRIBUTE_LIST gives the same tree whatever the "
    "thread count",
    "[mft][tree][threads]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const bool include_deleted = GENERATE(false, true);
  const bool recover = GENERATE(false, true);
  const auto listed =
      GENERATE(Tests::FakeListedState::Live, Tests::FakeListedState::Freed);
  const auto residency = GENERATE(Tests::FakeListResidency::Resident,
                                  Tests::FakeListResidency::NonResident);
  const auto high = GENERATE(Tests::FakeListedExtension::Genuine,
                             Tests::FakeListedExtension::Foreign,
                             Tests::FakeListedExtension::Base);
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithMftTreeAttributeList(high, residency,
                                                        listed);

  const ScanResult one =
      ScanWithThreads<S>(image, OptionsOf(include_deleted, recover), 1);
  const ScanResult many =
      ScanWithThreads<S>(image, OptionsOf(include_deleted, recover), 4);

  CHECK(many.parsed_by_threads > 0);
  CheckSameScan(one, many);
}

TEMPLATE_TEST_CASE_SIG(
    "A scan of the fixtures with a stray extension and the MFT tree gives the "
    "same tree whatever the thread count",
    "[mft][tree][threads]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const bool include_deleted = GENERATE(false, true);
  const bool recover = GENERATE(false, true);
  const std::vector<BYTE> image =
      GENERATE(Tests::BuildFakeNtfsImageWithMftTree(),
               Tests::BuildFakeNtfsImageWithMftTreeStrayExtension(),
               Tests::BuildFakeNtfsImageWithFreedFileTree(),
               Tests::BuildFakeNtfsImageWithMftExtensionRecord());

  const ScanResult one =
      ScanWithThreads<S>(image, OptionsOf(include_deleted, recover), 1);
  const ScanResult many =
      ScanWithThreads<S>(image, OptionsOf(include_deleted, recover), 4);

  CheckSameScan(one, many);
}

TEST_CASE(
    "A scan stopped by its progress callback is the same whatever the "
    "thread count",
    "[mft][tree][threads]") {
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithBusyMftScan(
      busy_record_count, busy_fragment_clusters);

  for (const size_t stop : {size_t{1}, size_t{2}, size_t{3}}) {
    INFO("stop at " << stop);
    CheckSameWhateverTheThreads<Cache::Strategy::NoCache>(
        image, OptionsOf(true, true), stop);
  }
}

TEST_CASE(
    "A scan names the slots the threads parsed and the ones it parsed "
    "itself",
    "[mft][tree][threads]") {
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithBusyMftScan(
      busy_record_count, busy_fragment_clusters);

  const ScanResult many = ScanWithThreads<Cache::Strategy::NoCache>(
      image, OptionsOf(true, true), 4);

  // Every slot past the system files but the ones with a
  // $LOGGED_UTILITY_STREAM (one in ten) and the zero-filled ones (one in ten,
  // unreadable but still taken from the threads).
  const size_t slots = busy_record_count - static_cast<size_t>(Mft::Idx::User);
  CHECK(many.parsed_by_threads > slots / 2);
  CHECK(many.parsed_by_threads < slots);
}

TEST_CASE(
    "A scan with one thread, or with none asked for on a small volume, "
    "takes nothing from other threads",
    "[mft][tree][threads]") {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithFragmentedMftScan(450);
  const VolumeOptions options = OptionsOf(true, true);

  CHECK(ScanWithThreads<Cache::Strategy::NoCache>(image, options, 1)
            .parsed_by_threads == 0);
  // 0 lets the scan choose, and a volume this small does not pay for threads.
  CHECK(ScanWithThreads<Cache::Strategy::NoCache>(image, options, 0)
            .parsed_by_threads == 0);
  CHECK(ScanWithThreads<Cache::Strategy::NoCache>(image, options, 2)
            .parsed_by_threads > 0);
}

namespace {

// The threads a raw attribute callback ran on.
std::mutex callback_mutex;
std::set<std::thread::id> callback_threads;

void RecordCallbackThread(const Attr::HeaderCommon& /*attr_head*/,
                          bool& discard) {
  const std::scoped_lock lock{callback_mutex};
  callback_threads.insert(std::this_thread::get_id());
  discard = false;
}

}  // namespace

TEST_CASE(
    "A raw attribute callback of the volume runs on the scanning thread "
    "alone",
    "[mft][tree][threads]") {
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithBusyMftScan(
      busy_record_count, busy_fragment_clusters);
  callback_threads.clear();
  NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<Tests::MemoryDiskReader>(image), OptionsOf(true, true));
  REQUIRE(volume.IsVolumeOK());
  CHECK_FALSE(volume.HasAttrRawCB());
  REQUIRE(volume.InstallAttrRawCB(Attr::Type::FileName, RecordCallbackThread));
  CHECK(volume.HasAttrRawCB());

  MftTree::ScanOptions scan;
  scan.threads = 4;
  static_cast<void>(Mft::SlotsParsedByThreadsForTests(true));
  const MftTree tree(volume, scan);

  CHECK(Mft::SlotsParsedByThreadsForTests(true) == 0);
  CHECK(callback_threads.size() == 1);
  CHECK(callback_threads.contains(std::this_thread::get_id()));

  volume.ClearAttrRawCB();
  CHECK_FALSE(volume.HasAttrRawCB());
}

namespace {

// Restores the limits of an automatic scan when it goes out of scope. verdict,
// when it holds a value, is what the probe decides, whatever it measures.
class AutoLimits {
 public:
  AutoLimits(ULONGLONG min_slots, ULONGLONG probe_slots,
             std::optional<bool> verdict = std::nullopt) {
    Mft::SetAutoThreadLimitsForTests(min_slots, probe_slots, verdict);
  }

  AutoLimits(const AutoLimits&) = delete;
  AutoLimits(AutoLimits&&) = delete;
  AutoLimits& operator=(const AutoLimits&) = delete;
  AutoLimits& operator=(AutoLimits&&) = delete;

  ~AutoLimits() {
    Mft::SetAutoThreadLimitsForTests(std::nullopt, std::nullopt);
  }
};

// A reader that makes each read wait, as a slow disk does.
class SlowDiskReader final : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  explicit SlowDiskReader(std::vector<BYTE> image) : inner_(std::move(image)) {}

  bool Open(std::wstring_view /*path*/) override { return false; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    std::this_thread::sleep_for(std::chrono::milliseconds(12));
    return inner_.ReadInto(addr, dest);
  }

 private:
  Tests::MemoryDiskReader inner_;
};

// Slots the scan took from threads, with the thread count left to it.
template <class Reader>
size_t AutomaticScanParsedByThreads(const std::vector<BYTE>& image) {
  const NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<Reader>(image), OptionsOf(true, true));
  REQUIRE(volume.IsVolumeOK());
  static_cast<void>(Mft::SlotsParsedByThreadsForTests(true));
  const MftTree tree(volume);
  return Mft::SlotsParsedByThreadsForTests(true);
}

}  // namespace

TEST_CASE("Threads pay for a scan that does not wait for its volume",
          "[mft][tree][threads]") {
  CHECK(Mft::ScanThreadsPay(0.0, 1.0));
  CHECK(Mft::ScanThreadsPay(0.1, 1.0));
  CHECK(Mft::ScanThreadsPay(0.2, 1.0));
  CHECK_FALSE(Mft::ScanThreadsPay(0.21, 1.0));
  CHECK_FALSE(Mft::ScanThreadsPay(0.5, 1.0));
  CHECK_FALSE(Mft::ScanThreadsPay(1.0, 1.0));
  // A probe that took no time says nothing.
  CHECK_FALSE(Mft::ScanThreadsPay(0.0, 0.0));
}

TEST_CASE("A scan that chooses its threads uses them once the probe says so",
          "[mft][tree][threads]") {
  const AutoLimits limits(1000, 1000, true);
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithBusyMftScan(
      busy_record_count, busy_fragment_clusters);

  const size_t parsed =
      AutomaticScanParsedByThreads<Tests::MemoryDiskReader>(image);

  // The first window is the probe, parsed by the scanning thread.
  const size_t slots = busy_record_count - static_cast<size_t>(Mft::Idx::User);
  CHECK(parsed > 0);
  CHECK(parsed < slots / 2 + (slots / 10));
}

TEST_CASE("A scan that chooses its threads keeps to one on a slow volume",
          "[mft][tree][threads]") {
  // The probe measures: each read of the volume waits, and loading a window is
  // most of the time of the probe.
  const AutoLimits limits(1000, 1000);
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithBusyMftScan(
      busy_record_count, busy_fragment_clusters);

  CHECK(AutomaticScanParsedByThreads<SlowDiskReader>(image) == 0);
}

TEST_CASE(
    "A scan that chooses its threads keeps to one below the volume size "
    "it asks for",
    "[mft][tree][threads]") {
  const AutoLimits limits(busy_record_count + 1, 1000, true);
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithBusyMftScan(
      busy_record_count, busy_fragment_clusters);

  CHECK(AutomaticScanParsedByThreads<Tests::MemoryDiskReader>(image) == 0);
}

TEST_CASE("A scan that chooses its threads is the scan of one thread",
          "[mft][tree][threads]") {
  const AutoLimits limits(1000, 1000, true);
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithBusyMftScan(
      busy_record_count, busy_fragment_clusters);

  const ScanResult one = ScanWithThreads<Cache::Strategy::NoCache>(
      image, OptionsOf(true, true), 1);
  const ScanResult automatic = ScanWithThreads<Cache::Strategy::NoCache>(
      image, OptionsOf(true, true), 0);

  CHECK(automatic.parsed_by_threads > 0);
  CheckSameScan(one, automatic);
}
