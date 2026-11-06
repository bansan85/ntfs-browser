#include <ntfs-browser/win-types.h>

#include <array>
#include <bit>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/alloc/types.h>
#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "alloc-test-support.h"
#include "bench-session-guard.h"
#include "bench/recorder.h"
#include "fake-ntfs-image.h"
#include "io/file-reader.h"
#include "memory-disk-reader.h"
#include "walk-comparison.h"

namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Support = NTFS_BROWSER_NAMESPACE::Tests::AllocSupport;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using Bench::Detail::KindScope;
using Bench::Detail::SliceScope;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Tests::FakeDataRun;

namespace {

using Clock = std::chrono::steady_clock;

// Slots of the scan fixture, as in phase-tests.cpp.
constexpr ULONGLONG list_record_count = 3000;
constexpr ULONGLONG list_fragment_clusters = 250;

// Slots of the walk fixture.
constexpr ULONGLONG walk_record_count = 100;

// Bytes of the image the direct reads touch.
constexpr size_t small_image_size = 4 * 65536;

void SpinFor(std::chrono::microseconds duration) {
  const auto until = Clock::now() + duration;
  while (Clock::now() < until) {
    // Nothing to do: the wait is the point.
  }
}

// A reader whose every read takes a while, and that notes nothing about it: the
// caller waited for the whole call.
class SlowDiskReader final : public IDiskReader {
 public:
  SlowDiskReader(std::vector<BYTE> data, std::chrono::microseconds delay)
      : inner_(std::move(data)), delay_(delay) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    SpinFor(delay_);
    return inner_.ReadInto(addr, dest);
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    return inner_.Size();
  }

 private:
  Tests::MemoryDiskReader inner_;
  std::chrono::microseconds delay_;
};

const Bench::KindStat& Kind(const Bench::Report& report, Bench::ReadKind kind) {
  return report.kinds.at(static_cast<size_t>(kind));
}

std::uint64_t Sum(const std::array<std::uint64_t, Bench::read_class_count>& v) {
  return std::accumulate(v.begin(), v.end(), std::uint64_t{0});
}

// What every kind of a report MUST satisfy, whatever read it counted: the
// histogram holds each read once, and the classes add up.
void CheckKindsAreConsistent(const Bench::Report& report) {
  std::uint64_t reads = 0;
  std::uint64_t bytes = 0;
  for (size_t index = 0; index < Bench::read_kind_count; index++) {
    const Bench::KindStat& stat = report.kinds.at(index);
    INFO("kind " << Bench::ReadKindName(static_cast<Bench::ReadKind>(index)));
    CHECK(stat.kind == static_cast<Bench::ReadKind>(index));
    CHECK(stat.sizes.count == stat.reads);
    CHECK(stat.sizes.sum == stat.bytes);
    CHECK(stat.sizes.max <= stat.bytes);
    CHECK(std::accumulate(stat.sizes.buckets.begin(), stat.sizes.buckets.end(),
                          std::uint64_t{0}) == stat.reads);
    CHECK(Sum(stat.by_class) == stat.reads);
    reads += stat.reads;
    bytes += stat.bytes;
  }
  CHECK(reads == report.requested_reads);
  CHECK(bytes == report.requested_bytes);
  CHECK(Sum(report.class_reads) == report.requested_reads);
}

size_t BucketOf(std::uint64_t value) {
  return value == 0 ? 0 : static_cast<size_t>(std::bit_width(value) - 1);
}

std::vector<BYTE> LiveStaleImage() {
  std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithFreedNonResidentFile(
      Support::stale_size, {FakeDataRun{.lcn = 40, .clusters = 4}});
  Support::FillStaleClusters(image, 13);
  return image;
}

}  // namespace

TEST_CASE("A kind scope declares its kind and restores the outer one",
          "[bench][kind]") {
  Tests::BenchSessionGuard session;
  CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::Other);
  {
    const KindScope outer(Bench::ReadKind::Boot);
    CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::Boot);
    {
      const KindScope inner(Bench::ReadKind::IndexBlock);
      CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::IndexBlock);
      {
        const KindScope innermost(Bench::ReadKind::Journal);
        CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::Journal);
      }
      CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::IndexBlock);
    }
    CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::Boot);
  }
  CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::Other);
}

TEST_CASE("A kind scope that yields leaves the kind an outer scope set",
          "[bench][kind]") {
  Tests::BenchSessionGuard session;
  {
    const KindScope general(Bench::ReadKind::Data, /*only_if_other=*/true);
    CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::Data);
    {
      const KindScope specific(Bench::ReadKind::BitmapWindow);
      CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::BitmapWindow);
      {
        const KindScope yielding(Bench::ReadKind::Data,
                                 /*only_if_other=*/true);
        CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::BitmapWindow);
      }
      CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::BitmapWindow);
    }
    CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::Data);
  }
  CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::Other);
}

TEST_CASE("A kind scope outside a session changes nothing", "[bench][kind]") {
  REQUIRE_FALSE(Bench::Enabled());
  const KindScope scope(Bench::ReadKind::Journal);
  CHECK(Bench::Detail::CurrentKind() == Bench::ReadKind::Other);
}

TEST_CASE("A read lands in the histogram bucket of its size",
          "[bench][kind][histogram]") {
  const std::vector<BYTE> image(small_image_size, BYTE{7});
  const Io::FileReader<Cache::Strategy::NoCache> file(
      std::make_unique<Tests::MemoryDiskReader>(image));
  const std::vector<size_t> lengths{1, 100, 4096, 5000, 65536};

  Tests::BenchSessionGuard session;
  {
    const KindScope kind(Bench::ReadKind::Data);
    for (const size_t length : lengths) {
      std::vector<BYTE> buffer(length);
      LARGE_INTEGER addr{.QuadPart = 0};
      REQUIRE(file.ReadInto(addr, buffer));
    }
  }
  const Bench::Report report = session.Finish();

  const Bench::KindStat& data = Kind(report, Bench::ReadKind::Data);
  CHECK(data.reads == lengths.size());
  CHECK(data.bytes ==
        std::accumulate(lengths.begin(), lengths.end(), std::uint64_t{0}));
  CHECK(data.sizes.count == lengths.size());
  CHECK(data.sizes.max == 65536);
  CHECK(data.sizes.buckets.at(0) == 1);
  CHECK(data.sizes.buckets.at(BucketOf(100)) == 1);
  // 4096 and 5000 share the bucket of the values from 4096 to 8191.
  CHECK(data.sizes.buckets.at(12) == 2);
  CHECK(data.sizes.buckets.at(16) == 1);
  // A reader that classifies nothing leaves every read Unclassified.
  CHECK(data.by_class.at(static_cast<size_t>(Bench::ReadClass::Unclassified)) ==
        lengths.size());
  CheckKindsAreConsistent(report);
  // Nothing else was read.
  CHECK(Kind(report, Bench::ReadKind::Other).reads == 0);
}

TEST_CASE("The reads of a volume open are Boot reads", "[bench][kind]") {
  Tests::BenchSessionGuard session;
  {
    const NtfsVolume<Cache::Strategy::NoCache> volume(
        std::make_unique<Tests::MemoryDiskReader>(Tests::BuildFakeNtfsImage()));
    REQUIRE(volume.IsVolumeOK());
  }
  const Bench::Report report = session.Finish();

  CHECK(Kind(report, Bench::ReadKind::Boot).reads > 0);
  CheckKindsAreConsistent(report);
}

TEMPLATE_TEST_CASE_SIG("The reads of a scan land in the kinds that made them",
                       "[bench][kind][tree]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithListMftScan(
      list_record_count, list_fragment_clusters);
  VolumeOptions options{.include_deleted = true, .recover_errors = true};
  // No record is held for the second pass: an extension record is read there.
  options.tuning.mft.max_held_record_bytes = 0;

  Tests::BenchSessionGuard session;
  {
    const NtfsVolume<S> volume(std::make_unique<Tests::MemoryDiskReader>(image),
                               options);
    REQUIRE(volume.IsVolumeOK());
    Mft::MftTree::ScanOptions scan;
    scan.threads = 1;
    const Mft::MftTree tree(volume, scan);
    REQUIRE(tree.Stats().complete);
  }
  const Bench::Report report = session.Finish();

  CHECK(Kind(report, Bench::ReadKind::Boot).reads > 0);
  CHECK(Kind(report, Bench::ReadKind::MftWindow).reads > 0);
  CHECK(Kind(report, Bench::ReadKind::AttributeList).reads > 0);
  // A FullCache volume finds the extension records in blocks it holds.
  if constexpr (S == Cache::Strategy::NoCache) {
    CHECK(Kind(report, Bench::ReadKind::ExtensionRecord).reads > 0);
  }
  // The scan does not walk indexes, nor read the data of a file.
  CHECK(Kind(report, Bench::ReadKind::IndexBlock).reads == 0);
  CHECK(Kind(report, Bench::ReadKind::Data).reads == 0);
  CheckKindsAreConsistent(report);
}

TEMPLATE_TEST_CASE_SIG("The reads of a walk land in the kinds that made them",
                       "[bench][kind][walk]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(walk_record_count);

  Tests::BenchSessionGuard session;
  {
    const NtfsVolume<S> volume(
        std::make_unique<Tests::MemoryDiskReader>(image),
        VolumeOptions{.include_deleted = true, .recover_errors = true});
    REQUIRE(volume.IsVolumeOK());
    const Tests::WalkResult walk = Tests::WalkVolume(volume, true);
    REQUIRE_FALSE(walk.entries.empty());
  }
  const Bench::Report report = session.Finish();

  CHECK(Kind(report, Bench::ReadKind::IndexBlock).reads > 0);
  CHECK(Kind(report, Bench::ReadKind::ProbeRun).reads > 0);
  CHECK(Kind(report, Bench::ReadKind::MftWindow).reads == 0);
  CheckKindsAreConsistent(report);
}

TEMPLATE_TEST_CASE_SIG("The reads of the bitmap and of file data have kinds",
                       "[bench][kind]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<FakeDataRun> allocated{
      FakeDataRun{.lcn = 40, .clusters = 2}};
  const std::vector<BYTE> bitmap_image = Support::WithBitmap(
      Tests::BuildFakeNtfsImageWithFreedFileTree(), allocated);

  SECTION("a bitmap window") {
    auto opened = Support::OpenVolume<S>(bitmap_image);
    Tests::BenchSessionGuard session;
    CHECK(opened.volume->GetClusterState(40) == Alloc::ClusterState::Allocated);
    const Bench::Report report = session.Finish();

    CHECK(Kind(report, Bench::ReadKind::BitmapWindow).reads > 0);
    CHECK(Kind(report, Bench::ReadKind::Data).reads == 0);
    CheckKindsAreConsistent(report);
  }

  SECTION("file data") {
    const auto opened = Support::OpenVolume<S>(LiveStaleImage());
    const auto record = Support::ParsedStale(opened);
    // The parse read the blocks that hold the stream: the data read must
    // reach the reader.
    opened.volume->MinifyCache();
    std::vector<BYTE> bytes(Support::stale_size);
    Tests::BenchSessionGuard session;
    const auto read = record->FindStream({})->ReadData(0, bytes);
    const Bench::Report report = session.Finish();

    REQUIRE(read.has_value());
    CHECK(*read == Support::stale_size);
    CHECK(Kind(report, Bench::ReadKind::Data).reads > 0);
    CHECK(Kind(report, Bench::ReadKind::BitmapWindow).reads == 0);
    CheckKindsAreConsistent(report);
  }
}

TEST_CASE("A slice takes out the slices nested in it", "[bench][slice]") {
  using std::chrono::milliseconds;
  Tests::BenchSessionGuard session;
  const auto began = Clock::now();
  {
    const SliceScope outer(Bench::Slice::Parse);
    SpinFor(milliseconds{10});
    {
      const SliceScope inner(Bench::Slice::Merge);
      SpinFor(milliseconds{40});
    }
    SpinFor(milliseconds{10});
  }
  const auto elapsed = Clock::now() - began;
  const Bench::Report report = session.Finish();

  const std::uint64_t parse =
      report.slices_ns.at(static_cast<size_t>(Bench::Slice::Parse));
  const std::uint64_t merge =
      report.slices_ns.at(static_cast<size_t>(Bench::Slice::Merge));
  const auto elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

  CHECK(merge >= 40'000'000);
  CHECK(parse >= 20'000'000);
  // Exclusive slices never add up to more than the time they ran in.
  CHECK(parse + merge <= elapsed_ns);
  CHECK(parse < merge);
  CHECK(report.slices_ns.at(static_cast<size_t>(Bench::Slice::PoolWait)) == 0);
  CHECK(report.slices_ns.at(static_cast<size_t>(Bench::Slice::LogReplay)) == 0);
}

TEST_CASE("Slices nested twice are each exclusive", "[bench][slice]") {
  using std::chrono::milliseconds;
  Tests::BenchSessionGuard session;
  const auto began = Clock::now();
  {
    const SliceScope outer(Bench::Slice::Parse);
    SpinFor(milliseconds{5});
    {
      const SliceScope middle(Bench::Slice::Merge);
      SpinFor(milliseconds{5});
      {
        const SliceScope inner(Bench::Slice::PoolWait);
        SpinFor(milliseconds{30});
      }
    }
  }
  const auto elapsed = Clock::now() - began;
  const Bench::Report report = session.Finish();

  const std::uint64_t parse =
      report.slices_ns.at(static_cast<size_t>(Bench::Slice::Parse));
  const std::uint64_t merge =
      report.slices_ns.at(static_cast<size_t>(Bench::Slice::Merge));
  const std::uint64_t wait =
      report.slices_ns.at(static_cast<size_t>(Bench::Slice::PoolWait));
  const auto elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

  CHECK(wait >= 30'000'000);
  CHECK(merge >= 5000000);
  CHECK(parse >= 5000000);
  CHECK(parse + merge + wait <= elapsed_ns);
  CHECK(merge < wait);
}

TEST_CASE("A slice repeats and adds its repeats", "[bench][slice]") {
  using std::chrono::milliseconds;
  Tests::BenchSessionGuard session;
  for (int round = 0; round < 3; round++) {
    const SliceScope scope(Bench::Slice::LogReplay);
    SpinFor(milliseconds{5});
  }
  const Bench::Report report = session.Finish();

  CHECK(report.slices_ns.at(static_cast<size_t>(Bench::Slice::LogReplay)) >=
        15'000'000);
}

TEST_CASE("A slice takes out the time its thread waited for the disk",
          "[bench][slice]") {
  using std::chrono::milliseconds;
  const Io::FileReader<Cache::Strategy::NoCache> file(
      std::make_unique<SlowDiskReader>(std::vector<BYTE>(small_image_size),
                                       milliseconds{30}));
  std::vector<BYTE> buffer(512);
  LARGE_INTEGER addr{.QuadPart = 0};

  Tests::BenchSessionGuard session;
  const auto began = Clock::now();
  {
    const SliceScope scope(Bench::Slice::Parse);
    SpinFor(milliseconds{10});
    REQUIRE(file.ReadInto(addr, buffer));
  }
  const auto elapsed = Clock::now() - began;
  const Bench::Report report = session.Finish();

  const std::uint64_t parse =
      report.slices_ns.at(static_cast<size_t>(Bench::Slice::Parse));
  const auto elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

  // The reader noted nothing, so the caller waited for the whole call.
  CHECK(report.read_wait_ns >= 30'000'000);
  CHECK(parse >= 10'000'000);
  CHECK(parse + report.read_wait_ns <= elapsed_ns);
  CHECK(parse < report.read_wait_ns);
}

TEST_CASE("A slice outside a session records nothing", "[bench][slice]") {
  {
    const SliceScope scope(Bench::Slice::Parse);
    SpinFor(std::chrono::milliseconds{1});
  }
  Tests::BenchSessionGuard session;
  const Bench::Report report = session.Finish();

  for (const std::uint64_t ns : report.slices_ns) {
    CHECK(ns == 0);
  }
}

TEST_CASE("Names of the report cover every enumerator", "[bench][names]") {
  for (size_t index = 0; index < Bench::read_kind_count; index++) {
    CHECK_FALSE(
        Bench::ReadKindName(static_cast<Bench::ReadKind>(index)).empty());
  }
  for (size_t index = 0; index < Bench::read_class_count; index++) {
    CHECK_FALSE(
        Bench::ReadClassName(static_cast<Bench::ReadClass>(index)).empty());
  }
  for (size_t index = 0; index < Bench::thread_role_count; index++) {
    CHECK_FALSE(
        Bench::ThreadRoleName(static_cast<Bench::ThreadRole>(index)).empty());
  }
  for (size_t index = 0; index < Bench::slice_count; index++) {
    CHECK_FALSE(Bench::SliceName(static_cast<Bench::Slice>(index)).empty());
  }
}
