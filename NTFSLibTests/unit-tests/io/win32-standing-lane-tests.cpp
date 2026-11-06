#include <ntfs-browser/win-types.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <set>
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
#include "io/win32-disk-reader.h"
#include "temp-dir.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
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

// One MiB, where the small ranges of a test that mixes sizes are put.
constexpr ULONGLONG mib_offset = ULONGLONG{1} << 20U;

// The largest offset: an interval that runs to it withdraws everything.
constexpr ULONGLONG max_offset = (std::numeric_limits<ULONGLONG>::max)();

// The reads, bytes and wasted bytes of the reads ahead of both lanes: these
// tests check the caps and the matching, which the two lanes share.
std::uint64_t AheadReads(const Bench::DiskStat& disk) {
  return disk.prefetch_reads + disk.standing_reads;
}

std::uint64_t AheadBytes(const Bench::DiskStat& disk) {
  return disk.prefetch_bytes + disk.standing_bytes;
}

std::uint64_t AheadWasted(const Bench::DiskStat& disk) {
  return disk.wasted_bytes + disk.standing_wasted_bytes;
}

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

// The offsets of the image whose reads the reader is told are not over yet.
std::set<ULONGLONG>& HeldReads() {
  static std::set<ULONGLONG> held;
  return held;
}

bool HeldReadProbe(const OVERLAPPED& overlapped) {
  const ULONGLONG offset =
      (ULONGLONG{overlapped.OffsetHigh} << 32U) | overlapped.Offset;
  return !HeldReads().contains(offset);
}

// A file in the system cache answers at once, so no read of it is ever seen in
// flight. For the life of the guard the reader calls a read in flight for as
// long as its offset is in HeldReads(); the read itself still finishes.
class HeldReadsGuard {
 public:
  HeldReadsGuard() { HeldReads().clear(); }

  ~HeldReadsGuard() {
    Io::SetReadDoneProbeForTests(previous_);
    HeldReads().clear();
  }

  HeldReadsGuard(const HeldReadsGuard&) = delete;
  HeldReadsGuard& operator=(const HeldReadsGuard&) = delete;
  HeldReadsGuard(HeldReadsGuard&&) = delete;
  HeldReadsGuard& operator=(HeldReadsGuard&&) = delete;

 private:
  Io::ReadDoneProbe previous_{Io::SetReadDoneProbeForTests(&HeldReadProbe)};
};

// Drives a reader with calls chosen by a seeded generator, and digests which
// of its reads were served from an announced range.
class RandomLaneDriver {
 public:
  RandomLaneDriver(const IDiskReader& reader, std::uint32_t seed)
      : reader_(reader), random_(seed) {}

  // A number below bound. Not a distribution: those are not the same on every
  // library.
  std::uint32_t Below(std::uint32_t bound) { return random_() % bound; }

  // Notes an offset that was announced, and gives it back.
  ULONGLONG Announce(ULONGLONG offset) {
    announced_.push_back(offset);
    return offset;
  }

  // Reads half a range or a whole one, most often at an announced offset, else
  // at the one that fallback gives.
  template <typename Fallback>
  void ReadSomewhere(const Fallback& fallback) {
    // Half a range leaves it, a whole one uses it up.
    const size_t length = Below(2) == 0 ? range_bytes / 2 : range_bytes;
    const bool announced_offset = !announced_.empty() && Below(4) != 0;
    const ULONGLONG offset =
        announced_offset ? announced_.at(Below(
                               static_cast<std::uint32_t>(announced_.size())))
                         : fallback();
    const ReadOutcome outcome = ReadAndNote(reader_, offset, length);
    CHECK(outcome.bytes_match);
    served_digest_ = (served_digest_ * 3) + (WasServed(outcome) ? 1 : 0);
    reads_served_ += WasServed(outcome) ? 1 : 0;
  }

  [[nodiscard]] std::uint64_t ServedDigest() const { return served_digest_; }

  [[nodiscard]] std::uint64_t ReadsServed() const { return reads_served_; }

 private:
  const IDiskReader& reader_;
  std::mt19937 random_;
  // The ranges announced so far: most reads fall in one of them.
  std::vector<ULONGLONG> announced_;
  std::uint64_t served_digest_{0};
  std::uint64_t reads_served_{0};
};

// Holds a read in flight at the first announcement of its offset, or never. A
// read that is over MUST stay over, so that no earlier read at the offset is
// called in flight again.
class FirstAnnouncementHold {
 public:
  void Hold(ULONGLONG offset, bool wanted) {
    if (seen_.insert(offset).second && wanted) {
      HeldReads().insert(offset);
    }
  }

 private:
  std::set<ULONGLONG> seen_;
};

// Ends one held read, chosen at random: any one can be over, at any step.
void EndOneHeldRead(RandomLaneDriver& driver) {
  if (HeldReads().empty()) {
    return;
  }
  auto victim = HeldReads().begin();
  std::advance(victim,
               driver.Below(static_cast<std::uint32_t>(HeldReads().size())));
  HeldReads().erase(victim);
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
  CHECK(AheadReads(report.disk) == 2);
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
  CHECK(AheadWasted(report.disk) == 0);
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
  CHECK(AheadWasted(report.disk) == 0);
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
  CHECK(AheadWasted(report.disk) == range_bytes);
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
    CHECK(AheadReads(report.disk) == 4);
    CHECK(AheadWasted(report.disk) == 4 * range_bytes);
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
  CHECK(AheadReads(report.disk) == 2);
}

TEST_CASE(
    "The standing lane leaves a share of the reads in flight to the first",
    "[io][win32-reader][standing]") {
  // A quarter of the reads, and at least one, stay with the first lane. The
  // reads of a file in the system cache finish at once, so the lane may start
  // more than its share: it never has more in flight.
  const auto row = GENERATE(table<size_t, size_t>(
      {{1, 0}, {2, 1}, {3, 2}, {4, 3}, {8, 6}, {32, 24}}));
  const size_t max_reads = std::get<0>(row);
  const size_t lane_reads = std::get<1>(row);
  INFO("max_reads " << max_reads);
  const Caps caps = CapsWith([max_reads](Tuning::ReaderTuning& tuning) {
    tuning.max_reads = max_reads;
  });
  const ImageFile image;

  SECTION("the lane alone starts the share it may take, and no more at once") {
    const auto reader = image.Open(caps);
    Tests::BenchSessionGuard session;
    AnnounceStanding(*reader, 0, 40);
    const Bench::Report report = session.Finish();
    CHECK(AheadReads(report.disk) >= lane_reads);
    CHECK(AheadReads(report.disk) <= 40);
    CHECK(report.disk.standing_reads == AheadReads(report.disk));
    CHECK(report.disk.prefetch_reads == 0);
  }

  SECTION("the first lane takes the rest, whatever the lane holds") {
    const auto reader = image.Open(caps);
    Tests::BenchSessionGuard session;
    AnnounceStanding(*reader, 0, 40);
    for (size_t index = 100; index < 140; index++) {
      reader->Prefetch(RangeAt(index), range_bytes);
    }
    const Bench::Report report = session.Finish();
    // The first lane starts the reads that it finds free, however many of the
    // lane's are done.
    CHECK(report.disk.prefetch_reads >= max_reads - lane_reads);
    CHECK(report.disk.prefetch_reads <= max_reads);
  }
}

TEST_CASE("A finished read of the standing lane holds no read in flight",
          "[io][win32-reader][standing]") {
  // Two reads in flight at most, one of them for the lane.
  const Caps caps =
      CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_reads = 2; });
  const ImageFile image;
  std::uint64_t started = 0;
  for (int attempt = 1; attempt <= 8 && started < 6; attempt++) {
    const auto reader = image.Open(caps);
    Tests::BenchSessionGuard session;
    // Each range starts once the read before it is over, and the finished
    // reads wait for their caller: six of them in all.
    for (size_t index = 0; index < 6; index++) {
      AnnounceStanding(*reader, index, 1);
      LetTheDiskRun(attempt);
    }
    started = session.Finish().disk.standing_reads;
  }
  CHECK(started == 6);
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
  CHECK(AheadReads(report.disk) == 4);
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

  CHECK(AheadReads(report.disk) == 0);
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

  CHECK(AheadReads(report.disk) == 2);
  CHECK(AheadBytes(report.disk) == 4 * range_bytes);
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
  CHECK(AheadReads(report.disk) == 1);
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
  CHECK(AheadReads(report.disk) == 3);
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
    // The bytes of one range in flight: the second range waits.
    const Caps caps = CapsWith(
        [](Tuning::ReaderTuning& tuning) { tuning.max_bytes = range_bytes; });
    Bench::ReadClass seen = Bench::ReadClass::Unclassified;
    for (int attempt = 1; attempt <= 8; attempt++) {
      const auto reader = image.Open(caps);
      AnnounceStanding(*reader, 0, 2);
      LetTheDiskRun(attempt);
      seen = ReadAndNote(*reader, RangeAt(0)).note.cls;
      if (seen == Bench::ReadClass::Window) {
        break;
      }
    }
    CHECK(seen == Bench::ReadClass::Window);
  }

  SECTION("a range the caps never started is read by the caller") {
    const Caps caps = CapsWith(
        [](Tuning::ReaderTuning& tuning) { tuning.max_bytes = range_bytes; });
    const auto reader = image.Open(caps);
    AnnounceStanding(*reader, 0, 2);
    const ReadOutcome held_back = ReadAndNote(*reader, RangeAt(1));
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

TEST_CASE("A slot that served a big standing range serves a tiny one next",
          "[io][win32-reader][standing]") {
  // Wider than the buffer a slot keeps: the slot gives it back when the lane
  // is done with it, and the next range gets a buffer of its own size.
  constexpr ULONGLONG big_range = ULONGLONG{256} * 1024;
  const ImageFile image;
  const auto reader = image.Open();
  Tests::BenchSessionGuard session;

  for (size_t round = 0; round < 4; round++) {
    reader->PrefetchStanding(round * big_range, big_range);
    const ReadOutcome big = ReadAndNote(*reader, round * big_range, big_range);
    reader->PrefetchStanding(mib_offset + RangeAt(round), range_bytes);
    const ReadOutcome tiny = ReadAndNote(*reader, mib_offset + RangeAt(round));
    CHECK(big.bytes_match);
    CHECK(tiny.bytes_match);
    CHECK(WasServed(tiny));
  }
}

TEST_CASE("The reads started do not depend on how the lane counts its own",
          "[io][win32-reader][standing]") {
  // At most 16 ranges in each lane: 16 + 16 reads never reach the 32 that may
  // be in flight, so every range starts whenever the disk answers, and the
  // counters are a function of the calls alone.
  const Caps caps = CapsWith(
      [](Tuning::ReaderTuning& tuning) { tuning.max_announced_ranges = 16; });
  const ImageFile image;
  const auto reader = image.Open(caps);
  Tests::BenchSessionGuard session;
  RandomLaneDriver driver(*reader, 2024);
  for (size_t step = 0; step < 600; step++) {
    const std::uint32_t kind = driver.Below(100);
    if (kind < 40) {
      reader->PrefetchStanding(driver.Announce(RangeAt(driver.Below(64))),
                               range_bytes);
    } else if (kind < 52) {
      reader->CancelStanding(RangeAt(driver.Below(64)) + driver.Below(3) * 512,
                             1 + driver.Below(4) * range_stride);
    } else if (kind < 64) {
      reader->Prefetch(driver.Announce(RangeAt(64 + driver.Below(32))),
                       range_bytes);
    } else if (kind < 68) {
      reader->CancelPrefetch();
    } else {
      driver.ReadSomewhere([&] { return RangeAt(driver.Below(96)); });
    }
  }
  const Bench::Report report = session.Finish();

  // Recorded with the reader that counted the lane's reads by visiting every
  // range it had started.
  CHECK(driver.ReadsServed() == 50);
  CHECK(driver.ServedDigest() == 6'341'278'128'170'066'284ULL);
  CHECK(report.disk.prefetch_reads == 64);
  CHECK(report.disk.prefetch_bytes == 262144);
  CHECK(report.disk.wasted_bytes == 249856);
  CHECK(report.disk.standing_reads == 93);
  CHECK(report.disk.standing_bytes == 380928);
  CHECK(report.disk.standing_wasted_bytes == 172032);
}

TEST_CASE("The lane starts a read as the ones in flight are over",
          "[io][win32-reader][standing]") {
  // Four reads in flight at most, three of them for the lane.
  const Caps caps =
      CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_reads = 4; });
  const ImageFile image;
  const HeldReadsGuard held;
  for (size_t index = 0; index < 8; index++) {
    HeldReads().insert(RangeAt(index));
  }
  const auto reader = image.Open(caps);
  // The reads that the lane starts while step runs.
  const auto lane_reads = [](const auto& step) {
    Tests::BenchSessionGuard session;
    step();
    return session.Finish().disk.standing_reads;
  };

  // The reads in flight keep the rest of the lane waiting.
  CHECK(lane_reads([&] { AnnounceStanding(*reader, 0, 6); }) == 3);

  HeldReads().erase(RangeAt(0));
  CHECK(lane_reads(
            [&] { reader->PrefetchStanding(RangeAt(6), range_bytes); }) == 1);

  // Range 3 is still in flight: the lane has room for two reads, and ranges 4
  // and 5 start.
  HeldReads().erase(RangeAt(1));
  HeldReads().erase(RangeAt(2));
  CHECK(lane_reads([&] { reader->CancelPrefetch(); }) == 2);
}

TEST_CASE("A withdrawn read in flight leaves its place to the next range",
          "[io][win32-reader][standing]") {
  const Caps caps =
      CapsWith([](Tuning::ReaderTuning& tuning) { tuning.max_reads = 4; });
  const ImageFile image;
  const HeldReadsGuard held;
  for (size_t index = 0; index < 8; index++) {
    HeldReads().insert(RangeAt(index));
  }
  const auto reader = image.Open(caps);
  Tests::BenchSessionGuard session;

  AnnounceStanding(*reader, 0, 5);
  // Ranges 0 to 2 are in flight. Withdrawing range 1 frees a place.
  reader->CancelStanding(RangeAt(1), range_bytes);
  // The slot of range 1 goes to range 3, and range 4 waits.
  HeldReads().erase(RangeAt(3));
  reader->CancelPrefetch();
  const Bench::Report report = session.Finish();
  CHECK(report.disk.standing_reads == 5);
  CHECK(report.disk.standing_wasted_bytes == range_bytes);
}

TEST_CASE("The reads started are the same whatever is in flight",
          "[io][win32-reader][standing]") {
  // What the reader did, recorded with the reader that counted the lane's
  // reads in flight by visiting every range it had started.
  struct Recorded {
    size_t max_reads;
    std::uint64_t reads_served;
    std::uint64_t served_digest;
    std::uint64_t prefetch_reads;
    std::uint64_t prefetch_bytes;
    std::uint64_t wasted_bytes;
    std::uint64_t standing_reads;
    std::uint64_t standing_bytes;
    std::uint64_t standing_wasted_bytes;
  };

  const auto recorded = GENERATE(values<Recorded>({
      {2, 73, 5'961'208'495'276'488'311ULL, 65, 266240, 258048, 158, 647168,
       344064},
      {3, 63, 6'346'597'772'769'804'089ULL, 76, 311296, 307200, 171, 700416,
       417792},
      {4, 65, 1'390'544'315'863'034'995ULL, 70, 286720, 278528, 144, 589824,
       303104},
      {8, 76, 6'857'536'744'735'337'862ULL, 80, 327680, 327680, 143, 585728,
       294912},
      {32, 59, 14'596'878'219'651'475'875ULL, 89, 364544, 364544, 154, 630784,
       376832},
  }));
  const size_t max_reads = recorded.max_reads;
  INFO("max_reads " << max_reads);
  // Few reads in flight at once, and enough ranges to fill the lane.
  const Caps caps = CapsWith([max_reads](Tuning::ReaderTuning& tuning) {
    tuning.max_reads = max_reads;
    tuning.max_announced_ranges = 24;
  });
  const ImageFile image;
  const HeldReadsGuard held;
  const auto reader = image.Open(caps);
  Tests::BenchSessionGuard session;
  RandomLaneDriver driver(*reader, static_cast<std::uint32_t>(max_reads));
  FirstAnnouncementHold hold;
  for (size_t step = 0; step < 800; step++) {
    const std::uint32_t kind = driver.Below(100);
    const ULONGLONG offset = RangeAt(driver.Below(48));
    if (kind < 30) {
      // Most of the reads are in flight until a later step ends them.
      hold.Hold(offset, driver.Below(4) != 0);
      reader->PrefetchStanding(driver.Announce(offset), range_bytes);
    } else if (kind < 42) {
      reader->CancelStanding(offset, 1 + driver.Below(4) * range_stride);
    } else if (kind < 52) {
      const ULONGLONG first = RangeAt(48 + driver.Below(16));
      hold.Hold(first, true);
      reader->Prefetch(driver.Announce(first), range_bytes);
    } else if (kind < 55) {
      reader->CancelPrefetch();
    } else if (kind < 75) {
      EndOneHeldRead(driver);
      reader->CancelPrefetch();
    } else {
      driver.ReadSomewhere([offset] { return offset; });
    }
  }
  const Bench::Report report = session.Finish();
  CHECK(driver.ReadsServed() == recorded.reads_served);
  CHECK(driver.ServedDigest() == recorded.served_digest);
  CHECK(report.disk.prefetch_reads == recorded.prefetch_reads);
  CHECK(report.disk.prefetch_bytes == recorded.prefetch_bytes);
  CHECK(report.disk.wasted_bytes == recorded.wasted_bytes);
  CHECK(report.disk.standing_reads == recorded.standing_reads);
  CHECK(report.disk.standing_bytes == recorded.standing_bytes);
  CHECK(report.disk.standing_wasted_bytes == recorded.standing_wasted_bytes);
}
