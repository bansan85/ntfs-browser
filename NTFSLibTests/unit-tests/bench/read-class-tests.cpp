#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "bench-session-guard.h"
#include "bench/recorder.h"
#include "counting-disk-reader.h"
#include "io/file-reader.h"
#include "memory-disk-reader.h"
#include "temp-dir.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using Caps = std::array<Tuning::ReaderTuning, Tuning::pattern_count>;

namespace {

// Bytes of the image file of the Windows tests: sixteen reads of 64 KiB.
constexpr size_t image_bytes = 16 * 65536;

// Bytes of the ranges the tests announce and read: one sector of 4096.
constexpr size_t range_bytes = 4096;

// Distance between two announced ranges, so that they do not touch.
constexpr size_t range_stride = 65536;

}  // namespace

TEST_CASE("A reader that does not classify gives Unclassified",
          "[bench][read-class]") {
  const std::vector<BYTE> image(image_bytes, BYTE{9});
  const Io::FileReader<Cache::Strategy::NoCache> file(
      std::make_unique<Tests::CountingDiskReader>(
          std::make_unique<Tests::MemoryDiskReader>(image)));
  std::vector<BYTE> buffer(range_bytes);
  LARGE_INTEGER addr{.QuadPart = 0};

  Tests::BenchSessionGuard session;
  // An announced read is no better known to a reader that ignores hints.
  file.Prefetch(0, range_bytes);
  REQUIRE(file.ReadInto(addr, buffer));
  addr.QuadPart = range_stride;
  REQUIRE(file.ReadInto(addr, buffer));
  const Bench::Report report = session.Finish();

  const auto unclassified = static_cast<size_t>(Bench::ReadClass::Unclassified);
  CHECK(report.requested_reads == 2);
  CHECK(report.class_reads.at(unclassified) == 2);
  for (size_t index = 0; index < Bench::read_class_count; index++) {
    if (index != unclassified) {
      CHECK(report.class_reads.at(index) == 0);
    }
  }
  // The whole call is a wait, for a reader that says nothing.
  CHECK(report.class_wait_ns.at(unclassified) == report.read_wait_ns);
  CHECK(report.disk.reads == 0);
}

TEST_CASE("The platform reader exists where the library has one",
          "[bench][read-class]") {
  const Tests::TempDir dir;
  const std::filesystem::path path =
      dir.Write("image.bin", std::string(range_bytes, 'x'));

  const std::unique_ptr<IDiskReader> reader =
      Bench::OpenPlatformReader(path.wstring(), Caps{});
#ifdef _WIN32
  CHECK(reader != nullptr);
#else
  CHECK(reader == nullptr);
#endif
  CHECK(Bench::OpenPlatformReader((dir.Path() / "missing.bin").wstring(),
                                  Caps{}) == nullptr);
}

#ifdef _WIN32

namespace {

// A file of image_bytes under a temp folder, and the readers opened on it.
class ImageFile {
 public:
  ImageFile() {
    std::string content(image_bytes, '\0');
    for (size_t i = 0; i < content.size(); i++) {
      content.at(i) = static_cast<char>((i * 13) + (i >> 9U));
    }
    path_ = dir_.Write("image.bin", content);
  }

  [[nodiscard]] std::unique_ptr<IDiskReader> Open(const Caps& caps) const {
    auto reader = Bench::OpenPlatformReader(path_.wstring(), caps);
    REQUIRE(reader != nullptr);
    return reader;
  }

 private:
  Tests::TempDir dir_;
  std::filesystem::path path_;
};

Caps CapsWithMaxReads(size_t max_reads) {
  Tuning::ReaderTuning caps;
  caps.max_reads = max_reads;
  Caps all;
  all.fill(caps);
  return all;
}

// What a read of reader at offset noted about itself.
Bench::Detail::ReadNote ReadAndNote(const IDiskReader& reader,
                                    std::uint64_t offset) {
  std::vector<BYTE> buffer(range_bytes);
  LARGE_INTEGER addr{};
  addr.QuadPart = static_cast<LONGLONG>(offset);
  Bench::Detail::ResetReadNote();
  REQUIRE(reader.ReadInto(addr, buffer));
  return Bench::Detail::LastReadNote();
}

// Announces count ranges of range_bytes from offset 0, range_stride apart.
void Announce(const IDiskReader& reader, size_t count) {
  for (size_t index = 0; index < count; index++) {
    reader.Prefetch(index * range_stride, range_bytes);
  }
}

// The class of the read of the first announced range, once the disk had time
// to finish it. A read that is not over yet is an Io read, so the wait grows
// until the class is the one expected, a few times.
Bench::ReadClass ClassOfFirstRangeAfterWait(const ImageFile& image,
                                            const Caps& caps, size_t announced,
                                            Bench::ReadClass expected) {
  Bench::ReadClass seen = Bench::ReadClass::Unclassified;
  for (int attempt = 1; attempt <= 8; attempt++) {
    const std::unique_ptr<IDiskReader> reader = image.Open(caps);
    Announce(*reader, announced);
    std::this_thread::sleep_for(std::chrono::milliseconds{25 * attempt});
    const Bench::Detail::ReadNote note = ReadAndNote(*reader, 0);
    REQUIRE(note.valid);
    seen = note.cls;
    if (seen == expected) {
      break;
    }
  }
  return seen;
}

}  // namespace

TEST_CASE("A read nobody announced is a Plan read", "[bench][read-class]") {
  const ImageFile image;
  const std::unique_ptr<IDiskReader> reader = image.Open(Caps{});
  Tests::BenchSessionGuard session;

  const Bench::Detail::ReadNote note = ReadAndNote(*reader, 0);

  CHECK(note.valid);
  CHECK(note.cls == Bench::ReadClass::Plan);
  CHECK(note.queue_depth >= 1);
}

TEST_CASE("A read outside every announced range is a Plan read",
          "[bench][read-class]") {
  const ImageFile image;
  const std::unique_ptr<IDiskReader> reader = image.Open(Caps{});
  Tests::BenchSessionGuard session;
  Announce(*reader, 2);

  const Bench::Detail::ReadNote note = ReadAndNote(*reader, 5 * range_stride);

  CHECK(note.cls == Bench::ReadClass::Plan);
}

TEST_CASE("A read of an announced range that is done and alone is Ready",
          "[bench][read-class]") {
  const ImageFile image;
  Tests::BenchSessionGuard session;

  CHECK(ClassOfFirstRangeAfterWait(image, Caps{}, 1, Bench::ReadClass::Ready) ==
        Bench::ReadClass::Ready);
}

TEST_CASE("A read the caller had to wait for is an Io read",
          "[bench][read-class]") {
  const ImageFile image;
  // One read at a time: the second range is not even started when its read
  // comes, and the first one is passed over.
  const std::unique_ptr<IDiskReader> reader = image.Open(CapsWithMaxReads(1));
  Tests::BenchSessionGuard session;
  Announce(*reader, 3);

  const Bench::Detail::ReadNote note = ReadAndNote(*reader, 2 * range_stride);

  CHECK(note.valid);
  CHECK(note.cls == Bench::ReadClass::Io);
}

TEST_CASE("A done read is a Window read once the caps hold ranges back",
          "[bench][read-class]") {
  const size_t max_reads = GENERATE(size_t{1}, size_t{2}, size_t{3});
  const size_t announced = GENERATE(size_t{1}, size_t{2}, size_t{3}, size_t{4});
  INFO("max_reads " << max_reads << ", announced " << announced);
  const ImageFile image;
  Tests::BenchSessionGuard session;

  // window_held_back turns true as soon as more ranges are announced than the
  // cap on the reads in flight allows.
  const Bench::ReadClass expected = announced > max_reads
                                        ? Bench::ReadClass::Window
                                        : Bench::ReadClass::Ready;
  CHECK(ClassOfFirstRangeAfterWait(image, CapsWithMaxReads(max_reads),
                                   announced, expected) == expected);
}

TEST_CASE("The caps by pattern decide which ranges are held back",
          "[bench][read-class]") {
  Caps caps;
  caps.at(static_cast<size_t>(Tuning::Pattern::LargeSequential)).max_reads = 1;
  caps.at(static_cast<size_t>(Tuning::Pattern::ScatteredGrouped)).max_reads =
      32;
  const ImageFile image;
  Tests::BenchSessionGuard session;

  SECTION("the first pattern holds the second range back") {
    CHECK(
        ClassOfFirstRangeAfterWait(image, caps, 2, Bench::ReadClass::Window) ==
        Bench::ReadClass::Window);
  }

  SECTION("the second pattern starts both") {
    Bench::ReadClass seen = Bench::ReadClass::Unclassified;
    for (int attempt = 1; attempt <= 8; attempt++) {
      const std::unique_ptr<IDiskReader> reader = image.Open(caps);
      reader->SetReadPattern(Tuning::Pattern::ScatteredGrouped);
      Announce(*reader, 2);
      std::this_thread::sleep_for(std::chrono::milliseconds{25 * attempt});
      seen = ReadAndNote(*reader, 0).cls;
      if (seen == Bench::ReadClass::Ready) {
        break;
      }
    }
    CHECK(seen == Bench::ReadClass::Ready);
  }
}

TEST_CASE("The classes and the disk side of a report add up",
          "[bench][read-class]") {
  const ImageFile image;
  const Io::FileReader<Cache::Strategy::NoCache> file(image.Open(Caps{}));
  std::vector<BYTE> buffer(range_bytes);
  LARGE_INTEGER addr{.QuadPart = 0};

  Tests::BenchSessionGuard session;
  // One announced range that is used, one unannounced read, and one range that
  // is announced and withdrawn.
  file.Prefetch(0, range_bytes);
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  REQUIRE(file.ReadInto(addr, buffer));
  addr.QuadPart = 4 * range_stride;
  REQUIRE(file.ReadInto(addr, buffer));
  file.Prefetch(8 * range_stride, range_bytes);
  file.CancelPrefetch();
  const Bench::Report report = session.Finish();

  const auto count = [&report](Bench::ReadClass cls) {
    return report.class_reads.at(static_cast<size_t>(cls));
  };
  CHECK(report.requested_reads == 2);
  CHECK(count(Bench::ReadClass::Plan) == 1);
  CHECK(count(Bench::ReadClass::Io) + count(Bench::ReadClass::Ready) +
            count(Bench::ReadClass::Window) ==
        1);
  CHECK(std::accumulate(report.class_reads.begin(), report.class_reads.end(),
                        std::uint64_t{0}) == report.requested_reads);

  // Two ranges were announced, and the unannounced read went to the disk too.
  CHECK(report.prefetch_calls == 2);
  CHECK(report.cancel_calls == 1);
  CHECK(report.disk.prefetch_reads == 2);
  CHECK(report.disk.prefetch_bytes == 2 * range_bytes);
  CHECK(report.disk.reads == 3);
  CHECK(report.disk.bytes == 3 * range_bytes);
  // The withdrawn range was read ahead for nothing.
  CHECK(report.disk.wasted_bytes == range_bytes);
  CHECK(report.disk.seeks >= 1);
  CHECK(report.disk.seek_bytes >= range_bytes);
  CHECK(std::accumulate(report.disk.queue_depth.begin(),
                        report.disk.queue_depth.end(),
                        std::uint64_t{0}) == report.disk.reads);
  CHECK(report.disk.sizes.count == report.disk.reads);
}

TEST_CASE("The disk reads of the standing lane are counted apart",
          "[bench][read-class][standing]") {
  const ImageFile image;
  const Io::FileReader<Cache::Strategy::NoCache> file(image.Open(Caps{}));
  std::vector<BYTE> buffer(range_bytes);
  LARGE_INTEGER addr{.QuadPart = 0};

  Tests::BenchSessionGuard session;
  // One range of the first queue that is used, two of the lane of which one is
  // used and one is withdrawn, and one read that nobody announced.
  file.Prefetch(0, range_bytes);
  file.PrefetchStanding(4 * range_stride, range_bytes);
  file.PrefetchStanding(8 * range_stride, range_bytes);
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  REQUIRE(file.ReadInto(addr, buffer));
  addr.QuadPart = 4 * range_stride;
  REQUIRE(file.ReadInto(addr, buffer));
  file.CancelStanding(8 * range_stride, range_bytes);
  addr.QuadPart = 12 * range_stride;
  REQUIRE(file.ReadInto(addr, buffer));
  const Bench::Report report = session.Finish();

  CHECK(report.prefetch_calls == 1);
  CHECK(report.standing_prefetch_calls == 2);
  CHECK(report.standing_prefetch_bytes == 2 * range_bytes);
  CHECK(report.standing_cancel_calls == 1);
  CHECK(report.cancel_calls == 0);

  const Bench::DiskStat& disk = report.disk;
  CHECK(disk.prefetch_reads == 1);
  CHECK(disk.prefetch_bytes == range_bytes);
  CHECK(disk.standing_reads == 2);
  CHECK(disk.standing_bytes == 2 * range_bytes);
  // Only the withdrawn range of the lane was read for nothing.
  CHECK(disk.wasted_bytes == 0);
  CHECK(disk.standing_wasted_bytes == range_bytes);
  // Every read of the disk is in exactly one of the three.
  CHECK(disk.reads == 4);
  CHECK(disk.bytes == 4 * range_bytes);
  CHECK(disk.reads - disk.prefetch_reads - disk.standing_reads == 1);
  CHECK(report.requested_reads == 3);
}

TEST_CASE("A withdrawn range of the first queue is not a standing waste",
          "[bench][read-class][standing]") {
  const ImageFile image;
  const Io::FileReader<Cache::Strategy::NoCache> file(image.Open(Caps{}));

  Tests::BenchSessionGuard session;
  file.Prefetch(0, range_bytes);
  file.PrefetchStanding(4 * range_stride, range_bytes);
  file.CancelPrefetch();
  file.CancelStanding(0, (std::numeric_limits<ULONGLONG>::max)());
  const Bench::Report report = session.Finish();

  CHECK(report.disk.wasted_bytes == range_bytes);
  CHECK(report.disk.standing_wasted_bytes == range_bytes);
  CHECK(report.disk.prefetch_reads == 1);
  CHECK(report.disk.standing_reads == 1);
}

#endif  // _WIN32
