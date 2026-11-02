#include <ntfs-browser/win-types.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "bench-session-guard.h"
#include "bench/recorder.h"
#include "temp-dir.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using Caps = std::array<Tuning::ReaderTuning, Tuning::pattern_count>;

namespace {

// Bytes of the image file: 512 sectors of 8 KiB.
constexpr size_t image_bytes = size_t{4} << 20U;

// Bytes of an announced range, a sector of 4096, and the distance between two
// of them: they never touch, and each is read by itself.
constexpr ULONGLONG range_bytes = 4096;
constexpr ULONGLONG range_stride = 8192;

// The largest offset: an interval that runs to it withdraws everything.
constexpr ULONGLONG max_offset = (std::numeric_limits<ULONGLONG>::max)();

// The byte of the image at index: it depends on its offset, so a shifted read
// cannot match by accident.
BYTE ByteAt(size_t index) {
  return static_cast<BYTE>((index * 13) + (index >> 9U));
}

// A file of image_bytes under a temp folder, and the readers opened on it.
class ImageFile {
 public:
  ImageFile() {
    std::string content(image_bytes, '\0');
    for (size_t index = 0; index < content.size(); index++) {
      content.at(index) = static_cast<char>(ByteAt(index));
    }
    path_ = dir_.Write("image.bin", content);
  }

  [[nodiscard]] std::unique_ptr<IDiskReader> Open(const Caps& caps = {}) const {
    auto reader = Bench::OpenPlatformReader(path_.wstring(), caps);
    REQUIRE(reader != nullptr);
    return reader;
  }

 private:
  Tests::TempDir dir_;
  std::filesystem::path path_;
};

// The caps of every pattern, with one field set by set.
template <typename Set>
Caps CapsWith(const Set& set) {
  Caps caps;
  for (Tuning::ReaderTuning& tuning : caps) {
    set(tuning);
  }
  return caps;
}

// What a read of length bytes at offset noted about itself, and whether it
// returned the bytes of the image.
struct ReadOutcome {
  Bench::Detail::ReadNote note;
  bool bytes_match;
};

ReadOutcome ReadAndNote(const IDiskReader& reader, ULONGLONG offset,
                        size_t length = range_bytes) {
  std::vector<BYTE> buffer(length);
  LARGE_INTEGER addr{};
  addr.QuadPart = static_cast<LONGLONG>(offset);
  Bench::Detail::ResetReadNote();
  REQUIRE(reader.ReadInto(addr, buffer));
  ReadOutcome outcome{.note = Bench::Detail::LastReadNote(),
                      .bytes_match = true};
  for (size_t index = 0; index < length; index++) {
    outcome.bytes_match =
        outcome.bytes_match && buffer.at(index) == ByteAt(offset + index);
  }
  return outcome;
}

// Whether a read was announced: served from a range, not read by the caller.
bool WasServed(const ReadOutcome& outcome) {
  return outcome.note.valid && outcome.note.cls != Bench::ReadClass::Plan;
}

// The offset of the range with this index.
ULONGLONG RangeAt(size_t index) { return index * range_stride; }

// Announces count ranges on the standing lane, from the range at first.
void AnnounceStanding(const IDiskReader& reader, size_t first, size_t count) {
  for (size_t index = first; index < first + count; index++) {
    reader.PrefetchStanding(RangeAt(index), range_bytes);
  }
}

// Gives the disk time to finish what was started.
void LetTheDiskRun(int attempt = 1) {
  std::this_thread::sleep_for(std::chrono::milliseconds{25 * attempt});
}

}  // namespace

TEST_CASE("A read inside a standing range is served from it",
          "[io][win32-reader][standing]") {
  const ImageFile image;
  const auto reader = image.Open();
  Tests::BenchSessionGuard session;
  AnnounceStanding(*reader, 0, 1);
  AnnounceStanding(*reader, 3, 1);

  const ReadOutcome inside = ReadAndNote(*reader, RangeAt(3) + 100, 200);
  const ReadOutcome whole = ReadAndNote(*reader, RangeAt(0));
  const Bench::Report report = session.Finish();

  CHECK(inside.bytes_match);
  CHECK(whole.bytes_match);
  CHECK(WasServed(inside));
  CHECK(WasServed(whole));
  // The caller read nothing itself: the two reads of the disk are the lane's.
  CHECK(report.disk.reads == 2);
  CHECK(report.disk.prefetch_reads == 2);
}

TEST_CASE("A read outside every standing range is a Plan read",
          "[io][win32-reader][standing]") {
  const ImageFile image;
  const auto reader = image.Open();
  Tests::BenchSessionGuard session;
  AnnounceStanding(*reader, 0, 2);

  const ReadOutcome outside = ReadAndNote(*reader, RangeAt(5));
  // A read that is only partly inside a range is not served from it.
  const ReadOutcome straddling =
      ReadAndNote(*reader, RangeAt(1) + range_bytes - 100, 200);

  CHECK(outside.bytes_match);
  CHECK(straddling.bytes_match);
  CHECK(outside.note.cls == Bench::ReadClass::Plan);
  CHECK(straddling.note.cls == Bench::ReadClass::Plan);
}

TEST_CASE("A read of either lane drops no range of the standing lane",
          "[io][win32-reader][standing]") {
  const ImageFile image;
  const auto reader = image.Open();
  Tests::BenchSessionGuard session;
  AnnounceStanding(*reader, 0, 3);
  reader->Prefetch(RangeAt(10), range_bytes);

  // The first lane, then the standing lane out of its order.
  const ReadOutcome first = ReadAndNote(*reader, RangeAt(10));
  const ReadOutcome last = ReadAndNote(*reader, RangeAt(2));
  const ReadOutcome middle = ReadAndNote(*reader, RangeAt(1));
  const ReadOutcome head = ReadAndNote(*reader, RangeAt(0));
  const Bench::Report report = session.Finish();

  for (const ReadOutcome& outcome : {first, last, middle, head}) {
    CHECK(outcome.bytes_match);
    CHECK(WasServed(outcome));
  }
  CHECK(report.disk.reads == 4);
  CHECK(report.disk.wasted_bytes == 0);
}

TEST_CASE("The first lane is tried before the standing lane",
          "[io][win32-reader][standing]") {
  const ImageFile image;
  const auto reader = image.Open();
  Tests::BenchSessionGuard session;
  // The lane holds a wider range than the first lane, over the same bytes.
  reader->PrefetchStanding(0, 4 * range_bytes);
  reader->Prefetch(range_bytes, range_bytes);

  const ReadOutcome first = ReadAndNote(*reader, range_bytes);
  // The first lane used its range up. The lane keeps its own.
  const ReadOutcome lane = ReadAndNote(*reader, 3 * range_bytes);
  const Bench::Report report = session.Finish();

  CHECK(first.bytes_match);
  CHECK(lane.bytes_match);
  CHECK(WasServed(first));
  CHECK(WasServed(lane));
  CHECK(report.disk.reads == 2);
  CHECK(report.disk.wasted_bytes == 0);
}

TEST_CASE("CancelPrefetch leaves the standing lane alone",
          "[io][win32-reader][standing]") {
  const ImageFile image;
  const auto reader = image.Open();
  Tests::BenchSessionGuard session;
  AnnounceStanding(*reader, 0, 2);
  reader->Prefetch(RangeAt(10), range_bytes);

  reader->CancelPrefetch();
  const ReadOutcome first = ReadAndNote(*reader, RangeAt(0));
  const ReadOutcome second = ReadAndNote(*reader, RangeAt(1));
  const Bench::Report report = session.Finish();

  CHECK(first.bytes_match);
  CHECK(second.bytes_match);
  CHECK(WasServed(first));
  CHECK(WasServed(second));
  // Only the range of the first lane was read for nothing.
  CHECK(report.disk.reads == 3);
  CHECK(report.disk.wasted_bytes == range_bytes);
}

TEST_CASE("CancelStanding withdraws the ranges that overlap the interval",
          "[io][win32-reader][standing]") {
  const ImageFile image;
  const auto reader = image.Open();
  Tests::BenchSessionGuard session;
  AnnounceStanding(*reader, 0, 4);

  SECTION("a length of 0 overlaps nothing") {
    reader->CancelStanding(RangeAt(1), 0);
    CHECK(WasServed(ReadAndNote(*reader, RangeAt(1))));
  }

  SECTION("an interval inside one range withdraws that range only") {
    reader->CancelStanding(RangeAt(1) + 10, 1);
    const ReadOutcome withdrawn = ReadAndNote(*reader, RangeAt(1));
    CHECK(withdrawn.bytes_match);
    CHECK(withdrawn.note.cls == Bench::ReadClass::Plan);
    CHECK(WasServed(ReadAndNote(*reader, RangeAt(0))));
    CHECK(WasServed(ReadAndNote(*reader, RangeAt(2))));
  }

  SECTION("an interval that ends where a range starts overlaps nothing") {
    reader->CancelStanding(RangeAt(1) - range_bytes, range_bytes);
    CHECK(WasServed(ReadAndNote(*reader, RangeAt(1))));
  }

  SECTION("an interval over several ranges withdraws each") {
    reader->CancelStanding(RangeAt(1) + 10, 2 * range_stride);
    CHECK(WasServed(ReadAndNote(*reader, RangeAt(0))));
    CHECK(ReadAndNote(*reader, RangeAt(1)).note.cls == Bench::ReadClass::Plan);
    CHECK(ReadAndNote(*reader, RangeAt(2)).note.cls == Bench::ReadClass::Plan);
    CHECK(ReadAndNote(*reader, RangeAt(3)).note.cls == Bench::ReadClass::Plan);
  }

  SECTION("an interval that runs to the largest offset withdraws the rest") {
    reader->CancelStanding(RangeAt(2), max_offset);
    CHECK(WasServed(ReadAndNote(*reader, RangeAt(1))));
    CHECK(ReadAndNote(*reader, RangeAt(2)).note.cls == Bench::ReadClass::Plan);
    reader->CancelStanding(0, max_offset);
    CHECK(ReadAndNote(*reader, RangeAt(0)).note.cls == Bench::ReadClass::Plan);
    // Nothing is left to withdraw.
    reader->CancelStanding(0, max_offset);
  }

  SECTION("the ranges that were read ahead for nothing are counted wasted") {
    LetTheDiskRun();
    reader->CancelStanding(0, max_offset);
    const Bench::Report report = session.Finish();
    CHECK(report.disk.prefetch_reads == 4);
    CHECK(report.disk.wasted_bytes == 4 * range_bytes);
  }
}

TEST_CASE("A range of the first lane equal to a standing range is ignored",
          "[io][win32-reader][standing]") {
  const ImageFile image;
  const auto reader = image.Open();
  Tests::BenchSessionGuard session;
  reader->PrefetchStanding(range_stride, 2 * range_bytes);

  reader->Prefetch(range_stride, 2 * range_bytes);
  // A range that only overlaps is another range.
  reader->Prefetch(range_stride, range_bytes);
  const ReadOutcome read = ReadAndNote(*reader, range_stride, 2 * range_bytes);
  const Bench::Report report = session.Finish();

  CHECK(read.bytes_match);
  CHECK(WasServed(read));
  CHECK(report.disk.prefetch_reads == 2);
}

TEST_CASE("The standing lane leaves a share of the read slots to the first",
          "[io][win32-reader][standing]") {
  // A quarter of the slots, and at least one, stay with the first lane.
  const auto row = GENERATE(table<size_t, size_t>(
      {{1, 0}, {2, 1}, {3, 2}, {4, 3}, {8, 6}, {32, 24}}));
  const size_t max_reads = std::get<0>(row);
  const size_t lane_reads = std::get<1>(row);
  INFO("max_reads " << max_reads);
  const Caps caps = CapsWith([max_reads](Tuning::ReaderTuning& tuning) {
    tuning.max_reads = max_reads;
  });
  const ImageFile image;

  SECTION("the lane alone starts the share it may take") {
    const auto reader = image.Open(caps);
    Tests::BenchSessionGuard session;
    AnnounceStanding(*reader, 0, 40);
    const Bench::Report report = session.Finish();
    CHECK(report.disk.prefetch_reads == lane_reads);
  }

  SECTION("the first lane takes the rest, whatever the lane holds") {
    const auto reader = image.Open(caps);
    Tests::BenchSessionGuard session;
    AnnounceStanding(*reader, 0, 40);
    for (size_t index = 100; index < 140; index++) {
      reader->Prefetch(RangeAt(index), range_bytes);
    }
    const Bench::Report report = session.Finish();
    CHECK(report.disk.prefetch_reads == max_reads);
  }
}

TEST_CASE("The lane holds at most max_announced_ranges ranges",
          "[io][win32-reader][standing]") {
  const Caps caps = CapsWith(
      [](Tuning::ReaderTuning& tuning) { tuning.max_announced_ranges = 3; });
  const ImageFile image;
  const auto reader = image.Open(caps);
  Tests::BenchSessionGuard session;

  AnnounceStanding(*reader, 0, 5);
  // The first lane has a cap of its own.
  reader->Prefetch(RangeAt(10), range_bytes);
  const ReadOutcome kept = ReadAndNote(*reader, RangeAt(2));
  const ReadOutcome ignored = ReadAndNote(*reader, RangeAt(4));
  const Bench::Report report = session.Finish();

  CHECK(kept.bytes_match);
  CHECK(ignored.bytes_match);
  CHECK(WasServed(kept));
  CHECK(ignored.note.cls == Bench::ReadClass::Plan);
  CHECK(report.disk.prefetch_reads == 4);
}

TEST_CASE("A range of the lane over the range cap is not announced",
          "[io][win32-reader][standing]") {
  const Caps caps = CapsWith([](Tuning::ReaderTuning& tuning) {
    tuning.max_range_bytes = 2 * range_bytes;
  });
  const ImageFile image;
  const auto reader = image.Open(caps);
  Tests::BenchSessionGuard session;

  reader->PrefetchStanding(0, 8 * range_bytes);
  reader->PrefetchStanding(range_stride, 0);
  reader->PrefetchStanding(max_offset - range_bytes, 2 * range_bytes);
  const Bench::Report report = session.Finish();

  CHECK(report.disk.prefetch_reads == 0);
}

TEST_CASE("The lane and the first lane share the bytes in flight",
          "[io][win32-reader][standing]") {
  const Caps caps = CapsWith(
      [](Tuning::ReaderTuning& tuning) { tuning.max_bytes = 4 * range_bytes; });
  const ImageFile image;
  const auto reader = image.Open(caps);
  Tests::BenchSessionGuard session;

  // The first one always starts, then the ones that fit. The lane stops at the
  // first range that does not fit, so that its reads start in order.
  reader->Prefetch(RangeAt(10), 2 * range_bytes);
  reader->PrefetchStanding(RangeAt(20), 2 * range_bytes);
  reader->PrefetchStanding(RangeAt(30), 2 * range_bytes);
  reader->PrefetchStanding(RangeAt(40), range_bytes);
  const Bench::Report report = session.Finish();

  CHECK(report.disk.prefetch_reads == 2);
  CHECK(report.disk.prefetch_bytes == 4 * range_bytes);
}

TEST_CASE("A lone range over the byte cap still starts on the lane",
          "[io][win32-reader][standing]") {
  const Caps caps = CapsWith(
      [](Tuning::ReaderTuning& tuning) { tuning.max_bytes = range_bytes; });
  const ImageFile image;
  const auto reader = image.Open(caps);
  Tests::BenchSessionGuard session;

  reader->PrefetchStanding(0, 8 * range_bytes);
  const ReadOutcome read = ReadAndNote(*reader, 0, 8 * range_bytes);
  const Bench::Report report = session.Finish();

  CHECK(read.bytes_match);
  CHECK(WasServed(read));
  CHECK(report.disk.prefetch_reads == 1);
}

TEST_CASE("The lane starts under the caps of the pattern in force",
          "[io][win32-reader][standing]") {
  Caps caps;
  caps.at(static_cast<size_t>(Tuning::Pattern::ScatteredGrouped)).max_reads = 1;
  const ImageFile image;
  const auto reader = image.Open(caps);
  Tests::BenchSessionGuard session;

  reader->SetReadPattern(Tuning::Pattern::ScatteredGrouped);
  AnnounceStanding(*reader, 0, 3);
  reader->SetReadPattern(Tuning::Pattern::LargeSequential);
  const Bench::Report report = session.Finish();

  // One read in all under the first pattern, and the first lane keeps it: the
  // three ranges start once the caps allow more.
  CHECK(report.disk.prefetch_reads == 3);
}

TEST_CASE("A lane read that is done is classified like a first lane one",
          "[io][win32-reader][standing][read-class]") {
  const ImageFile image;
  Tests::BenchSessionGuard session;

  SECTION("done, and nothing held back, is Ready") {
    Bench::ReadClass seen = Bench::ReadClass::Unclassified;
    for (int attempt = 1; attempt <= 8; attempt++) {
      const auto reader = image.Open();
      AnnounceStanding(*reader, 0, 1);
      LetTheDiskRun(attempt);
      seen = ReadAndNote(*reader, RangeAt(0)).note.cls;
      if (seen == Bench::ReadClass::Ready) {
        break;
      }
    }
    CHECK(seen == Bench::ReadClass::Ready);
  }

  SECTION("done, with a range held back by the caps, is Window") {
    // Four slots leave three to the lane: the fourth range waits.
    const Caps caps =
        CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_reads = 4; });
    Bench::ReadClass seen = Bench::ReadClass::Unclassified;
    for (int attempt = 1; attempt <= 8; attempt++) {
      const auto reader = image.Open(caps);
      AnnounceStanding(*reader, 0, 4);
      LetTheDiskRun(attempt);
      seen = ReadAndNote(*reader, RangeAt(0)).note.cls;
      if (seen == Bench::ReadClass::Window) {
        break;
      }
    }
    CHECK(seen == Bench::ReadClass::Window);
  }

  SECTION("a range the caps never started is read by the caller") {
    const Caps caps =
        CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_reads = 4; });
    const auto reader = image.Open(caps);
    AnnounceStanding(*reader, 0, 4);
    const ReadOutcome held_back = ReadAndNote(*reader, RangeAt(3));
    CHECK(held_back.bytes_match);
    CHECK(held_back.note.cls == Bench::ReadClass::Plan);
  }
}

TEST_CASE("A reader destroyed with standing reads in flight frees them",
          "[io][win32-reader][standing]") {
  // Big ranges, so that the reads are still in flight when the reader goes.
  constexpr ULONGLONG big_range = ULONGLONG{256} * 1024;
  const ImageFile image;
  for (int round = 0; round < 3; round++) {
    const auto reader = image.Open();
    for (size_t index = 0; index < 16; index++) {
      reader->PrefetchStanding(index * big_range, big_range);
    }
    reader->Prefetch(0, big_range / 2);
    // Some of the lane is read, the rest is dropped with the reader.
    REQUIRE(ReadAndNote(*reader, 0, 4096).bytes_match);
  }
  SUCCEED();
}

TEST_CASE("The bytes equal a plain read, whatever the lanes hold",
          "[io][win32-reader][standing]") {
  const ImageFile image;
  const auto reader = image.Open();
  std::mt19937 random(7);

  // Standing ranges of mixed sizes, some wide, and reads at any offset,
  // inside them or not, aligned or not.
  for (size_t index = 0; index < 40; index++) {
    reader->PrefetchStanding(index * 50000 + (random() % 3000),
                             4096 + (random() % 20000));
    if (index % 5 == 0) {
      reader->Prefetch(index * 50000, 8192);
    }
  }
  for (size_t round = 0; round < 300; round++) {
    const ULONGLONG offset = random() % (image_bytes - 5000);
    const size_t length = 1 + (random() % 4000);
    if (round % 50 == 49) {
      reader->CancelStanding(offset, 30000);
    }
    CHECK(ReadAndNote(*reader, offset, length).bytes_match);
  }
}
