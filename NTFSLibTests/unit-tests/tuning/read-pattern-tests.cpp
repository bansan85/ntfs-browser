#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/alloc/cluster-account.h>
#include <ntfs-browser/attr/mask.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/io/file-record.h>
#include <ntfs-browser/io/read-ahead-disk-reader.h>
#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/alloc/alloc-test-support.h>
#include <ntfs-browser/test/alloc/cluster-account-support.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "knob-effect-support.h"

namespace NTFS_BROWSER_NAMESPACE {

class IndexEntryView;

namespace Io {

enum class RecordStanding : std::uint8_t;

}  // namespace Io
}  // namespace NTFS_BROWSER_NAMESPACE

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
namespace Account = NTFS_BROWSER_NAMESPACE::Tests::AccountSupport;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using NTFS_BROWSER_NAMESPACE::IndexEntryView;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::KnobEffect::Tuned;
using Patterns = std::vector<Tuning::Pattern>;

namespace {

// A reader over memory that notes every pattern it is told, into a list the
// test owns.
class PatternReader final : public IDiskReader {
 public:
  PatternReader(std::vector<BYTE> image, std::shared_ptr<Patterns> seen)
      : inner_(std::move(image)), seen_(std::move(seen)) {}

  bool Open(std::wstring_view /*path*/) override { return true; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    return inner_.ReadInto(addr, dest);
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    return inner_.Size();
  }

  void SetReadPattern(Tuning::Pattern pattern) const override {
    seen_->push_back(pattern);
  }

 private:
  TestDiskReaders::MemoryDiskReader inner_;
  std::shared_ptr<Patterns> seen_;
};

// The patterns in order, each once however many times it was set in a row.
Patterns Runs(const Patterns& patterns) {
  Patterns runs;
  for (const Tuning::Pattern pattern : patterns) {
    if (runs.empty() || runs.back() != pattern) {
      runs.push_back(pattern);
    }
  }
  return runs;
}

bool Has(const Patterns& patterns, Tuning::Pattern pattern) {
  return std::ranges::find(patterns, pattern) != patterns.end();
}

// The patterns a scan of image sets on the reader of its volume.
Patterns ScanPatterns(const std::vector<BYTE>& image,
                      const VolumeOptions& options,
                      const Mft::MftTree::ScanOptions& scan = {}) {
  const auto seen = std::make_shared<Patterns>();
  const NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<PatternReader>(image, seen), options);
  REQUIRE(volume.IsVolumeOK());
  seen->clear();
  const Mft::MftTree tree(volume, scan);
  return *seen;
}

}  // namespace

TEST_CASE("ReadAheadDiskReader passes a read pattern on to the reader it wraps",
          "[tuning][read-pattern]") {
  const auto seen = std::make_shared<Patterns>();
  const Io::ReadAheadDiskReader reader(
      std::make_unique<PatternReader>(std::vector<BYTE>(8192), seen));

  reader.SetReadPattern(Tuning::Pattern::ScatteredGrouped);
  reader.SetReadPattern(Tuning::Pattern::LargeData);
  reader.SetReadPattern(Tuning::Pattern::LargeSequential);

  CHECK(*seen == Patterns{Tuning::Pattern::ScatteredGrouped,
                          Tuning::Pattern::LargeData,
                          Tuning::Pattern::LargeSequential});
}

TEMPLATE_TEST_CASE_SIG("NtfsVolume passes a read pattern to its reader",
                       "[tuning][read-pattern]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const bool read_ahead = GENERATE(false, true);
  INFO("read ahead " << read_ahead);
  const auto seen = std::make_shared<Patterns>();
  const NtfsVolume<S> volume(
      std::make_unique<PatternReader>(
          TestFakeImage::BuildFakeNtfsImageWithMftTree(), seen),
      VolumeOptions{.read_ahead_bytes = read_ahead ? 4096U : 0U});
  REQUIRE(volume.IsVolumeOK());
  seen->clear();

  volume.SetReadPattern(Tuning::Pattern::LargeData);
  volume.SetReadPattern(Tuning::Pattern::DependentSmall);

  CHECK(*seen ==
        Patterns{Tuning::Pattern::LargeData, Tuning::Pattern::DependentSmall});
}

TEST_CASE("A scan reads its first pass in M1 and its second in M2",
          "[tuning][read-pattern][tree]") {
  const unsigned threads = GENERATE(1U, 3U);
  INFO("threads " << threads);
  Mft::MftTree::ScanOptions scan;
  scan.threads = threads;

  const Patterns seen = ScanPatterns(
      TestFakeImage::BuildFakeNtfsImageWithListMftScan(3000, 250),
      VolumeOptions{.include_deleted = true, .recover_errors = true}, scan);

  REQUIRE_FALSE(seen.empty());
  CHECK(seen.front() == Tuning::Pattern::LargeSequential);
  CHECK(Has(seen, Tuning::Pattern::ScatteredGrouped));
  CHECK_FALSE(Has(seen, Tuning::Pattern::LargeData));
  // The first pass ends before the second begins.
  const Patterns runs = Runs(seen);
  REQUIRE(runs.size() >= 2);
  CHECK(runs.at(0) == Tuning::Pattern::LargeSequential);
  CHECK(runs.at(1) == Tuning::Pattern::ScatteredGrouped);
}

TEST_CASE("The content check reads in M4", "[tuning][read-pattern][tree]") {
  Account::Volume volume;
  volume.bitmap_options.volume_clusters = 400;
  volume.Put(22, Account::Freed(100, Account::Stream({{100, 4}}, 4096)));
  Mft::MftTree::ScanOptions scan;
  scan.cluster_account = Alloc::ClusterAccountLevel::Counts;
  scan.verify_data = Alloc::DataCheckMode::Full;

  const Patterns seen =
      ScanPatterns(volume.Build(), Tests::AllocSupport::DeletedOptions(), scan);

  CHECK(Has(seen, Tuning::Pattern::LargeData));
  CHECK(Has(seen, Tuning::Pattern::LargeSequential));
}

TEST_CASE("The batched walk probes in M2", "[tuning][read-pattern]") {
  const auto seen = std::make_shared<Patterns>();
  const NtfsVolume<Cache::Strategy::NoCache> volume(
      std::make_unique<PatternReader>(
          TestFakeImage::BuildFakeNtfsImageWithIndexedMftScan(100), seen),
      VolumeOptions{});
  REQUIRE(volume.IsVolumeOK());
  Io::FileRecord<Cache::Strategy::NoCache> root(volume);
  root.SetAttrMask(NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexRoot |
                   NTFS_BROWSER_NAMESPACE::Attr::Mask::IndexAllocation);
  REQUIRE(root.ParseFileRecord(static_cast<ULONGLONG>(Mft::Idx::Root)));
  REQUIRE(root.ParseAttrs());
  seen->clear();

  root.TraverseSubEntriesWithStanding(
      [](const IndexEntryView&, Io::RecordStanding, void*) {}, nullptr);

  CHECK(Has(*seen, Tuning::Pattern::ScatteredGrouped));
  CHECK_FALSE(Has(*seen, Tuning::Pattern::LargeSequential));
}

TEST_CASE("A tuned scan sets the patterns of an untuned one",
          "[tuning][read-pattern][tree]") {
  const std::vector<BYTE> image =
      TestFakeImage::BuildFakeNtfsImageWithListMftScan(3000, 250);
  const VolumeOptions base{.include_deleted = true, .recover_errors = true};

  const Patterns plain = ScanPatterns(image, base);
  const Patterns tuned =
      ScanPatterns(image, Tuned(base, {{"mft.window_bytes", 4096},
                                       {"mft.max_held_record_bytes", 0},
                                       {"reader.max_reads", 2}}));

  CHECK(Runs(tuned) == Runs(plain));
}
