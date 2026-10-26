#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "corpus-test-support.h"
#include "counting-disk-reader.h"
#include "external-mft-support.h"
#include "memory-disk-reader.h"
#include "nps-ntfs1-test-support.h"
#include "scan-comparison.h"
#include "test-log-sink.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Fs = std::filesystem;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;

namespace {

// Where the kept $MFT files of dfir_ntfs sit under its root.
constexpr std::string_view mft_folder = "test_data";

// Scans the standalone $MFT file with threads threads. The volume is opened
// after the log is emptied, so that its opening counts too.
Tests::ScanResult ScanMftFile(const Fs::path& file,
                              const VolumeOptions& options, unsigned threads) {
  static_cast<void>(Tests::TakeCapturedLog());
  const std::unique_ptr<NtfsVolume<Cache::Strategy::NoCache>> volume =
      Tests::OpenStandaloneMftVolume(file, options);
  return Tests::CaptureScan(*volume, nullptr, threads);
}

}  // namespace

TEST_CASE("A small real $MFT scans the same whatever the thread count",
          "[corpus][mft][threads]") {
  const std::string_view name = GENERATE(
      as<std::string_view>{}, "slack.mft", "slack-2.mft", "slack-3.mft",
      "deleted.mft", "orphan.mft", "unicode.mft", "mapping_pairs_1.mft");
  const bool include_deleted = GENERATE(false, true);
  const bool recover = GENERATE(false, true);
  const Fs::path file = Tests::RequireCorpusFile(
      Tests::DfirNtfsDir() / mft_folder / std::string(name));
  const VolumeOptions options{.include_deleted = include_deleted,
                              .recover_errors = recover};

  const Tests::ScanResult one = ScanMftFile(file, options, 1);
  const Tests::ScanResult many = ScanMftFile(file, options, 4);

  INFO(std::string(name) << " deleted " << include_deleted << " recover "
                         << recover);
  CHECK_FALSE(one.tree.empty());
  CHECK(many.parsed_by_threads > 0);
  Tests::CheckSameScan(one, many);
}

TEST_CASE("The NIST hacking case $MFT scans the same whatever the thread count",
          "[corpus][mft][threads]") {
  const Fs::path file = Tests::RequireCorpusFile(
      Tests::DfirNtfsDir() / mft_folder / "nist-hacking-case.mft");
  const bool recover = GENERATE(false, true);
  const VolumeOptions options{.include_deleted = true,
                              .recover_errors = recover};

  const Tests::ScanResult one = ScanMftFile(file, options, 1);
  const Tests::ScanResult several = ScanMftFile(file, options, 3);
  const Tests::ScanResult many = ScanMftFile(file, options, 8);

  // Over 12 000 slots: three windows, and the last one short.
  CHECK(one.tree.size() > 1000000);
  CHECK(several.parsed_by_threads > 10000);
  // Its files with an $ATTRIBUTE_LIST, parsed by threads in the second pass.
  CHECK(several.waiting_by_threads > 0);
  CHECK(one.waiting_by_threads == 0);
  Tests::CheckSameScan(one, several);
  Tests::CheckSameScan(one, many);
}

TEST_CASE("The NPS ntfs1 image scans the same whatever the thread count",
          "[corpus][nps][threads]") {
  constexpr std::string_view trace = "nps-ntfs1-scan-threads";
  Tests::RequireCorpusImage(Tests::ntfs1_image, trace);
  const bool include_deleted = GENERATE(false, true);
  const VolumeOptions options{.include_deleted = include_deleted,
                              .recover_errors = true};
  const auto scan = [&](unsigned threads) {
    static_cast<void>(Tests::TakeCapturedLog());
    auto counting = std::make_unique<Tests::CountingDiskReader>(
        Tests::OpenCorpusImage(Tests::ntfs1_image, 0, trace));
    const Tests::CountingDiskReader* const reader = counting.get();
    const NtfsVolume<Cache::Strategy::NoCache> volume(std::move(counting),
                                                      options);
    REQUIRE(volume.IsVolumeOK());
    return Tests::CaptureScan(volume, reader, threads);
  };

  const Tests::ScanResult one = scan(1);
  const Tests::ScanResult many = scan(4);

  CHECK_FALSE(one.tree.empty());
  CHECK(many.parsed_by_threads > 0);
  Tests::CheckSameScan(one, many);
}

TEMPLATE_TEST_CASE_SIG(
    "A volume with attribute lists scans the same whatever the thread count",
    "[corpus][threads]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const Fs::path file = Tests::RequireCorpusFile(
      Tests::DfirNtfsDir() / mft_folder / "ntfs3_attrlist.raw");
  const std::vector<BYTE> bytes = Tests::ReadCorpusFile(file);
  const bool include_deleted = GENERATE(false, true);
  const bool recover = GENERATE(false, true);
  const VolumeOptions options{.include_deleted = include_deleted,
                              .recover_errors = recover};
  const auto scan = [&](unsigned threads) {
    static_cast<void>(Tests::TakeCapturedLog());
    auto counting = std::make_unique<Tests::CountingDiskReader>(
        std::make_unique<Tests::MemoryDiskReader>(bytes));
    const Tests::CountingDiskReader* const reader = counting.get();
    const NtfsVolume<S> volume(std::move(counting), options);
    REQUIRE(volume.IsVolumeOK());
    return Tests::CaptureScan(volume, reader, threads);
  };

  const Tests::ScanResult one = scan(1);
  CHECK_FALSE(one.tree.empty());
  for (const unsigned threads : {2U, 8U}) {
    INFO("threads " << threads);
    const Tests::ScanResult many = scan(threads);
    Tests::CheckSameScan(one, many);
  }
}
