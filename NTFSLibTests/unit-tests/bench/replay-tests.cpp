#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/mft/tree.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/disk-readers/throttled-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/bench-session-guard.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::IDiskReader;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Call = TestDiskReaders::CountingDiskReader::Call;
using CallKind = TestDiskReaders::CountingDiskReader::Kind;

namespace {

// Slots of the scan fixture whose log the replays use.
constexpr ULONGLONG list_record_count = 3000;
constexpr ULONGLONG list_fragment_clusters = 250;

// One operation a reader received, as text: kind, offset and length.
using Op = std::string;

std::string Describe(char kind, std::uint64_t offset, std::uint64_t length) {
  return std::string(1, kind) + " " + std::to_string(offset) + " " +
         std::to_string(length);
}

// A reader that notes every read, hint and pattern change it receives, in
// order. CountingDiskReader does not note the patterns.
class ScriptRecorder final : public IDiskReader {
 public:
  explicit ScriptRecorder(size_t size) : data_(size, BYTE{3}) {}

  bool Open(std::wstring_view /*path*/) override { return true; }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    ops_.push_back(
        Describe('R', static_cast<std::uint64_t>(addr.QuadPart), dest.size()));
    const auto offset = static_cast<size_t>(addr.QuadPart);
    if (offset + dest.size() > data_.size()) {
      return false;
    }
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset),
                dest.size(), dest.begin());
    return true;
  }

  [[nodiscard]] std::optional<ULONGLONG> Size() const override {
    return data_.size();
  }

  void Prefetch(ULONGLONG offset, ULONGLONG length) const override {
    ops_.push_back(Describe('P', offset, length));
  }

  void CancelPrefetch() const override { ops_.emplace_back("C"); }

  void PrefetchStanding(ULONGLONG offset, ULONGLONG length) const override {
    ops_.push_back(Describe('S', offset, length));
  }

  void CancelStanding(ULONGLONG offset, ULONGLONG length) const override {
    ops_.push_back(Describe('X', offset, length));
  }

  void SetReadPattern(Tuning::Pattern pattern) const override {
    ops_.push_back("T " + std::to_string(static_cast<unsigned>(pattern)));
  }

  [[nodiscard]] const std::vector<Op>& Ops() const { return ops_; }

 private:
  std::vector<BYTE> data_;
  mutable std::vector<Op> ops_;
};

Bench::LogEvent Event(Bench::EventType type, std::uint64_t offset = 0,
                      std::uint64_t length = 0) {
  Bench::LogEvent event;
  event.type = type;
  event.offset = offset;
  event.length = length;
  return event;
}

Bench::LogEvent ReadEvent(std::uint64_t offset, std::uint64_t length,
                          Bench::ReadKind kind = Bench::ReadKind::Other) {
  Bench::LogEvent event = Event(Bench::EventType::Read, offset, length);
  event.kind = kind;
  return event;
}

Bench::LogEvent PatternEvent(Tuning::Pattern pattern) {
  return Event(Bench::EventType::Pattern, 0,
               static_cast<std::uint64_t>(pattern));
}

// Four reads with hints, withdrawals and pattern changes between them.
Bench::ReadLog HandMadeLog() {
  Bench::ReadLog log;
  log.events = {
      Event(Bench::EventType::PhaseBegin, 0, 1),
      PatternEvent(Tuning::Pattern::LargeSequential),
      Event(Bench::EventType::Prefetch, 0, 100),
      Event(Bench::EventType::Prefetch, 4096, 100),
      ReadEvent(0, 100),
      PatternEvent(Tuning::Pattern::ScatteredGrouped),
      ReadEvent(4096, 100),
      Event(Bench::EventType::Cancel),
      Event(Bench::EventType::Prefetch, 8192, 100),
      ReadEvent(8192, 100),
      Event(Bench::EventType::PhaseEnd, 0, 1),
      ReadEvent(12288, 100),
  };
  return log;
}

// Reads with the lane's calls between them: ranges announced on the standing
// lane, one withdrawn, and the whole lane withdrawn at the end.
Bench::ReadLog StandingLog() {
  Bench::ReadLog log;
  log.events = {
      Event(Bench::EventType::StandingPrefetch, 4096, 100),
      Event(Bench::EventType::Prefetch, 0, 100),
      Event(Bench::EventType::StandingPrefetch, 8192, 100),
      ReadEvent(0, 100),
      Event(Bench::EventType::StandingCancel, 4096, 100),
      ReadEvent(8192, 100),
      Event(Bench::EventType::Cancel),
      Event(Bench::EventType::StandingCancel, 0,
            (std::numeric_limits<std::uint64_t>::max)()),
      ReadEvent(12288, 100),
  };
  return log;
}

// The log, calls and report of a scan of the list fixture inside a session.
struct RecordedScan {
  Bench::ReadLog log;
  Bench::Report report;
  std::vector<Call> calls;
  std::vector<BYTE> image;
};

RecordedScan RecordScan() {
  RecordedScan recorded;
  recorded.image = TestFakeImage::BuildFakeNtfsImageWithListMftScan(
      list_record_count, list_fragment_clusters);
  Tests::BenchSessionGuard session;
  {
    auto reader = std::make_unique<TestDiskReaders::CountingDiskReader>(
        std::make_unique<TestDiskReaders::MemoryDiskReader>(recorded.image));
    const TestDiskReaders::CountingDiskReader* const counting = reader.get();
    const NtfsVolume<Cache::Strategy::NoCache> volume(
        std::move(reader),
        VolumeOptions{.include_deleted = true, .recover_errors = true});
    REQUIRE(volume.IsVolumeOK());
    Mft::MftTree::ScanOptions scan;
    scan.threads = 1;
    const Mft::MftTree tree(volume, scan);
    REQUIRE(tree.Stats().complete);
    recorded.calls = counting->Calls();
  }
  recorded.report = session.Finish();
  recorded.log = Bench::TakeReadLog();
  return recorded;
}

// The calls that move data or hints: not the queries.
std::vector<Call> MovingCalls(const std::vector<Call>& calls) {
  std::vector<Call> moving;
  std::ranges::copy_if(calls, std::back_inserter(moving), [](const Call& call) {
    return call.kind == CallKind::Read || call.kind == CallKind::Prefetch ||
           call.kind == CallKind::CancelPrefetch;
  });
  return moving;
}

bool SameCalls(const std::vector<Call>& left, const std::vector<Call>& right) {
  return std::ranges::equal(left, right, [](const Call& lhs, const Call& rhs) {
    return lhs.kind == rhs.kind && lhs.offset == rhs.offset &&
           lhs.length == rhs.length;
  });
}

size_t CountEvents(const Bench::ReadLog& log, Bench::EventType type) {
  return static_cast<size_t>(
      std::ranges::count_if(log.events, [type](const Bench::LogEvent& event) {
        return event.type == type;
      }));
}

}  // namespace

TEST_CASE("An as-recorded replay makes the calls of the log in order",
          "[bench][replay]") {
  const ScriptRecorder reader(16384);

  const Bench::ReplayResult result = Bench::Replay(reader, HandMadeLog(), {});

  const std::vector<Op> expected{
      "T 0",        "P 0 100", "P 4096 100", "R 0 100",    "T 1",
      "R 4096 100", "C",       "P 8192 100", "R 8192 100", "R 12288 100"};
  CHECK(reader.Ops() == expected);
  CHECK(result.reads == 4);
  CHECK(result.read_bytes == 400);
  CHECK(result.failed_reads == 0);
  CHECK(result.prefetches == 3);
  CHECK(result.cancels == 1);
}

TEST_CASE("An all-announced replay drops the hints and announces every read",
          "[bench][replay]") {
  const ScriptRecorder reader(16384);
  const Bench::ReplayOptions options{.mode = Bench::ReplayMode::AllAnnounced,
                                     .announce_ahead = 1};

  const Bench::ReplayResult result =
      Bench::Replay(reader, HandMadeLog(), options);

  // Two reads are announced before the first, then one more before each.
  const std::vector<Op> expected{
      "T 0",        "P 0 100",    "P 4096 100",  "R 0 100",    "T 1",
      "P 8192 100", "R 4096 100", "P 12288 100", "R 8192 100", "R 12288 100"};
  CHECK(reader.Ops() == expected);
  CHECK(result.reads == 4);
  CHECK(result.prefetches == 4);
  CHECK(result.cancels == 0);
  CHECK(result.failed_reads == 0);
}

TEST_CASE("An as-recorded replay sends the standing lane calls in place",
          "[bench][replay][standing]") {
  const ScriptRecorder reader(16384);

  const Bench::ReplayResult result = Bench::Replay(reader, StandingLog(), {});

  const std::vector<Op> expected{
      "S 4096 100",
      "P 0 100",
      "S 8192 100",
      "R 0 100",
      "X 4096 100",
      "R 8192 100",
      "C",
      "X 0 " + std::to_string((std::numeric_limits<std::uint64_t>::max)()),
      "R 12288 100"};
  CHECK(reader.Ops() == expected);
  CHECK(result.reads == 3);
  CHECK(result.prefetches == 1);
  CHECK(result.cancels == 1);
  CHECK(result.standing_prefetches == 2);
  CHECK(result.standing_cancels == 2);
}

TEST_CASE("An as-recorded replay shows the lane calls to a counting reader",
          "[bench][replay][standing]") {
  auto counting = std::make_unique<TestDiskReaders::CountingDiskReader>(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(
          std::vector<BYTE>(16384, BYTE{1})));
  const TestDiskReaders::CountingDiskReader* const reader = counting.get();

  static_cast<void>(Bench::Replay(*counting, StandingLog(), {}));

  const std::vector<CallKind> kinds{CallKind::PrefetchStanding,
                                    CallKind::Prefetch,
                                    CallKind::PrefetchStanding,
                                    CallKind::Read,
                                    CallKind::CancelStanding,
                                    CallKind::Read,
                                    CallKind::CancelPrefetch,
                                    CallKind::CancelStanding,
                                    CallKind::Read};
  std::vector<CallKind> seen;
  for (const Call& call : reader->Calls()) {
    seen.push_back(call.kind);
  }
  CHECK(seen == kinds);
  CHECK(reader->Calls().at(0).offset == 4096);
  CHECK(reader->Calls().at(0).length == 100);
}

TEST_CASE("An all-announced replay drops the standing lane calls",
          "[bench][replay][standing]") {
  auto counting = std::make_unique<TestDiskReaders::CountingDiskReader>(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(
          std::vector<BYTE>(16384, BYTE{1})));
  const TestDiskReaders::CountingDiskReader* const reader = counting.get();
  const Bench::ReplayOptions options{.mode = Bench::ReplayMode::AllAnnounced,
                                     .announce_ahead = 1};

  const Bench::ReplayResult result =
      Bench::Replay(*counting, StandingLog(), options);

  for (const Call& call : reader->Calls()) {
    CHECK(call.kind != CallKind::PrefetchStanding);
    CHECK(call.kind != CallKind::CancelStanding);
  }
  CHECK(result.reads == 3);
  CHECK(result.standing_prefetches == 0);
  CHECK(result.standing_cancels == 0);
  CHECK(result.prefetches == 3);
  CHECK(result.cancels == 0);
}

TEST_CASE("A replay of a log with no standing lane call counts none",
          "[bench][replay][standing]") {
  const ScriptRecorder reader(16384);

  const Bench::ReplayResult result = Bench::Replay(reader, HandMadeLog(), {});

  CHECK(result.standing_prefetches == 0);
  CHECK(result.standing_cancels == 0);
}

TEST_CASE("An all-announced replay keeps the pattern events",
          "[bench][replay]") {
  const ScriptRecorder reader(16384);

  const Bench::ReplayResult result = Bench::Replay(
      reader, HandMadeLog(), {.mode = Bench::ReplayMode::AllAnnounced});

  const auto patterns = static_cast<size_t>(std::ranges::count_if(
      reader.Ops(), [](const Op& op) { return op.starts_with("T "); }));
  CHECK(patterns == 2);
  CHECK(std::ranges::count(reader.Ops(), Op{"C"}) == 0);
  CHECK(result.cancels == 0);
  // With the default window, every read is announced before the first.
  CHECK(reader.Ops().at(1) == "P 0 100");
}

TEST_CASE("A replay of an empty log does nothing", "[bench][replay]") {
  const ScriptRecorder reader(4096);

  const Bench::ReplayResult result = Bench::Replay(reader, Bench::ReadLog{});

  CHECK(reader.Ops().empty());
  CHECK(result.reads == 0);
  CHECK(result.prefetches == 0);
}

TEST_CASE("A replay counts the reads that fail or are too long",
          "[bench][replay]") {
  const ScriptRecorder reader(4096);
  Bench::ReadLog log;
  log.events = {
      ReadEvent(0, 100),
      // Past the end of the medium: the reader refuses.
      ReadEvent(4000, 500),
      // Longer than a replay will buffer: not made at all.
      ReadEvent(0, std::uint64_t{1} << 40U),
      // An address no LONGLONG holds.
      ReadEvent(~std::uint64_t{0}, 10),
      ReadEvent(100, 100),
  };

  Bench::ReplayResult result;
  REQUIRE_NOTHROW(result = Bench::Replay(reader, log, {}));

  CHECK(result.reads == 5);
  CHECK(result.failed_reads == 3);
  CHECK(result.read_bytes == 200);
  // Only the reads that could be made reached the reader.
  CHECK(reader.Ops().size() == 3);
}

TEST_CASE("An as-recorded replay reads what the original run read",
          "[bench][replay]") {
  const RecordedScan recorded = RecordScan();
  const std::vector<Call> original = MovingCalls(recorded.calls);
  REQUIRE(original.size() > 100);

  auto counting = std::make_unique<TestDiskReaders::CountingDiskReader>(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(recorded.image));
  const TestDiskReaders::CountingDiskReader* const reader = counting.get();
  const Bench::ReplayResult result = Bench::Replay(*counting, recorded.log, {});

  // The log holds what reached the reader: the replay makes the same reads
  // and the same hints, in the same order.
  CHECK(SameCalls(MovingCalls(reader->Calls()), original));
  CHECK(result.failed_reads == 0);
  CHECK(result.reads == CountEvents(recorded.log, Bench::EventType::Read));
  CHECK(result.prefetches ==
        CountEvents(recorded.log, Bench::EventType::Prefetch));
  CHECK(result.cancels == CountEvents(recorded.log, Bench::EventType::Cancel));
  CHECK(result.reads == recorded.report.requested_reads);
  CHECK(result.read_bytes == recorded.report.requested_bytes);
  CHECK(result.wall_ns > 0);
}

TEST_CASE("A replay inside a session is timed like the real run",
          "[bench][replay]") {
  const RecordedScan recorded = RecordScan();
  const TestDiskReaders::MemoryDiskReader reader(recorded.image);

  Tests::BenchSessionGuard session;
  const Bench::ReplayResult result = Bench::Replay(reader, recorded.log, {});
  const Bench::Report report = session.Finish();

  CHECK(report.requested_reads == recorded.report.requested_reads);
  CHECK(report.requested_bytes == recorded.report.requested_bytes);
  CHECK(result.reads == report.requested_reads);
  // Each replayed read keeps the kind of the read it stands for.
  for (size_t index = 0; index < Bench::read_kind_count; index++) {
    INFO("kind " << Bench::ReadKindName(static_cast<Bench::ReadKind>(index)));
    CHECK(report.kinds.at(index).reads ==
          recorded.report.kinds.at(index).reads);
    CHECK(report.kinds.at(index).bytes ==
          recorded.report.kinds.at(index).bytes);
  }
}

TEST_CASE("A replay over a simulated disk serves the reads it announced",
          "[bench][replay]") {
  const RecordedScan recorded = RecordScan();
  const Bench::ReplayMode mode =
      GENERATE(Bench::ReplayMode::AsRecorded, Bench::ReplayMode::AllAnnounced);
  INFO("all announced " << (mode == Bench::ReplayMode::AllAnnounced));
  TestDiskReaders::SimulatedClock clock;
  const TestDiskReaders::ThrottledDiskReader reader(
      std::make_unique<TestDiskReaders::MemoryDiskReader>(recorded.image),
      TestDiskReaders::DiskModel{}, clock);

  const Bench::ReplayResult result =
      Bench::Replay(reader, recorded.log, {.mode = mode});

  const TestDiskReaders::ThrottleStats& stats = reader.Stats();
  CHECK(result.failed_reads == 0);
  CHECK(result.reads == recorded.report.requested_reads);
  CHECK(stats.served_reads + stats.unannounced_reads == result.reads);
  CHECK(stats.served_reads > 0);
  CHECK(clock.NowNs() > 0);
  CHECK(stats.disk_bytes > 0);
  if (mode == Bench::ReplayMode::AllAnnounced) {
    CHECK(result.cancels == 0);
    CHECK(result.prefetches > 0);
  }
}

TEST_CASE("Replaying the same log over the same simulated disk is repeatable",
          "[bench][replay]") {
  const RecordedScan recorded = RecordScan();
  std::array<std::uint64_t, 2> elapsed{};
  for (std::uint64_t& slot : elapsed) {
    TestDiskReaders::SimulatedClock clock;
    const TestDiskReaders::ThrottledDiskReader reader(
        std::make_unique<TestDiskReaders::MemoryDiskReader>(recorded.image),
        TestDiskReaders::DiskModel{}, clock);
    static_cast<void>(Bench::Replay(reader, recorded.log, {}));
    slot = clock.NowNs();
  }

  CHECK(elapsed.at(0) == elapsed.at(1));
  CHECK(elapsed.at(0) > 0);
}
