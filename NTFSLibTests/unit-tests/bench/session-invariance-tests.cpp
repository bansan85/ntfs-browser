#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/bench-session-guard.h>
#include <ntfs-browser/test/support/scan-comparison.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/test/support/walk-comparison.h>
#include <ntfs-browser/tuning/tuning.h>
#include <ntfs-browser/volume-options.h>

#include "io/file-reader.h"

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Call = Tests::CountingDiskReader::Call;
using Kind = Tests::CountingDiskReader::Kind;

namespace {

// Bytes of the image the reader script reads: not a multiple of the 64 KiB
// block, so the last FullCache block is short.
constexpr size_t script_image_size = (3 * 64 * 1024) + 5000;

// Slots of the scan fixture: 3000 hold about 200 files with a list in a
// cluster of its own, so both passes of the scan read.
constexpr ULONGLONG list_record_count = 3000;
constexpr ULONGLONG list_fragment_clusters = 250;

// Slots of the walk fixture.
constexpr ULONGLONG walk_record_count = 100;

size_t CountKind(const std::vector<Call>& calls, Kind kind) {
  return static_cast<size_t>(std::ranges::count_if(
      calls, [kind](const Call& call) { return call.kind == kind; }));
}

// Drops the Size() queries from calls and returns how many there were. A
// session asks the reader for the size of the volume once, to put it in its
// report. A query reads nothing and announces nothing.
size_t TakeSizeQueries(std::vector<Call>& calls) {
  const size_t before = calls.size();
  std::erase_if(calls,
                [](const Call& call) { return call.kind == Kind::Size; });
  return before - calls.size();
}

std::vector<Call> ReadsOf(const std::vector<Call>& calls) {
  std::vector<Call> reads;
  std::ranges::copy_if(calls, std::back_inserter(reads), [](const Call& call) {
    return call.kind == Kind::Read;
  });
  return reads;
}

bool SameCallList(const std::vector<Call>& left,
                  const std::vector<Call>& right) {
  return std::ranges::equal(left, right, [](const Call& lhs, const Call& rhs) {
    return lhs.kind == rhs.kind && lhs.offset == rhs.offset &&
           lhs.length == rhs.length;
  });
}

// Where two call lists part: the index and the calls there, or empty.
std::string CallDivergence(const std::vector<Call>& left,
                           const std::vector<Call>& right) {
  const auto at =
      std::ranges::mismatch(left, right, [](const Call& lhs, const Call& rhs) {
        return lhs.kind == rhs.kind && lhs.offset == rhs.offset &&
               lhs.length == rhs.length;
      });
  if (at.in1 == left.end() && at.in2 == right.end()) {
    return {};
  }
  const auto describe = [](const auto& it, const auto& end) {
    return it == end ? std::string{"end"}
                     : std::format("kind {} offset {} length {}",
                                   static_cast<int>(it->kind), it->offset,
                                   it->length);
  };
  return std::format("at {} of {} and {}: {} / {}", at.in1 - left.begin(),
                     left.size(), right.size(), describe(at.in1, left.end()),
                     describe(at.in2, right.end()));
}

// What a script of calls on a FileReader left: the calls the reader under it
// received, what each read returned, and whether it succeeded.
struct ScriptRun {
  std::vector<Call> calls;
  std::vector<BYTE> bytes;
  std::vector<bool> succeeded;
};

template <Cache::Strategy S>
void ReadTo(const Io::FileReader<S>& reader, LONGLONG offset, DWORD length,
            ScriptRun& run) {
  LARGE_INTEGER addr{.QuadPart = offset};
  const auto got = reader.Read(addr, length);
  run.succeeded.push_back(got.has_value());
  if (got) {
    const std::span<const BYTE> view = *got;
    run.bytes.insert(run.bytes.end(), view.begin(), view.end());
  }
}

// Reads, hints, withdrawals and pattern changes in the order a scan makes
// them: in a block, across blocks, a hint on cached blocks, a read past the
// end of the medium.
template <Cache::Strategy S>
ScriptRun RunScript(const std::vector<BYTE>& image) {
  auto counting = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(image));
  const Tests::CountingDiskReader* const reader = counting.get();
  const Io::FileReader<S> file(std::move(counting));

  ScriptRun run;
  file.SetReadPattern(Tuning::Pattern::LargeSequential);
  file.Prefetch(0, (3 * 64 * 1024) + 100);
  file.CancelPrefetch();
  file.Prefetch(0, 100);
  ReadTo(file, 0, 100, run);
  ReadTo(file, (64 * 1024) - 50, 100, run);
  file.SetReadPattern(Tuning::Pattern::DependentSmall);
  ReadTo(file, 70000, 10, run);
  // The blocks the cache holds are left out of the hint.
  file.Prefetch(0, 200000);
  ReadTo(file, 150000, 40000, run);
  file.SetReadPattern(Tuning::Pattern::ScatteredGrouped);
  file.Prefetch(190000, 70000);
  LARGE_INTEGER addr{.QuadPart = 250000};
  std::vector<BYTE> direct(300);
  run.succeeded.push_back(file.ReadInto(addr, direct));
  run.bytes.insert(run.bytes.end(), direct.begin(), direct.end());
  ReadTo(file, static_cast<LONGLONG>(image.size()) - 50, 50, run);
  ReadTo(file, static_cast<LONGLONG>(image.size()) - 20, 100, run);
  ReadTo(file, 100, 0, run);
  // The standing lane: a range, a read inside it, a withdrawal of a part of
  // it, and of the whole lane.
  file.PrefetchStanding(100000, 5000);
  file.PrefetchStanding(20000, 100);
  ReadTo(file, 100100, 40, run);
  file.CancelStanding(20000, 1);
  file.CancelStanding(0, (std::numeric_limits<ULONGLONG>::max)());
  file.CancelPrefetch();

  run.calls = reader->Calls();
  return run;
}

std::vector<BYTE> PatternImage(size_t size) {
  std::vector<BYTE> image(size);
  for (size_t i = 0; i < size; i++) {
    // i < size by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    image[i] = static_cast<BYTE>((i * 7) + (i >> 8U));
  }
  return image;
}

// The reads of a log, in order.
std::vector<Call> LoggedReads(const Bench::ReadLog& log) {
  std::vector<Call> reads;
  for (const Bench::LogEvent& event : log.events) {
    if (event.type == Bench::EventType::Read) {
      reads.push_back(
          {.kind = Kind::Read, .offset = event.offset, .length = event.length});
    }
  }
  return reads;
}

// A scan of the list fixture over a counting reader: the volume is opened and
// scanned in this call, so a session started before it sees the open too.
template <Cache::Strategy S>
Tests::ScanResult ScanList(const std::vector<BYTE>& image,
                           const VolumeOptions& options, unsigned threads) {
  static_cast<void>(Tests::TakeCapturedLog());
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(image));
  const Tests::CountingDiskReader* const counting = reader.get();
  const NtfsVolume<S> volume(std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());
  return Tests::CaptureScan(volume, counting, threads);
}

// What a batched walk showed and asked of the reader.
struct WalkRun {
  Tests::WalkResult walk;
  std::vector<Call> calls;
};

template <Cache::Strategy S>
WalkRun WalkIndexed(const std::vector<BYTE>& image,
                    const VolumeOptions& options) {
  static_cast<void>(Tests::TakeCapturedLog());
  auto reader = std::make_unique<Tests::CountingDiskReader>(
      std::make_unique<Tests::MemoryDiskReader>(image));
  const Tests::CountingDiskReader* const counting = reader.get();
  const NtfsVolume<S> volume(std::move(reader), options);
  REQUIRE(volume.IsVolumeOK());
  WalkRun run;
  run.walk = Tests::WalkVolume(volume, true);
  run.calls = counting->Calls();
  return run;
}

}  // namespace

TEST_CASE("No Bench session runs unless a test starts one",
          "[bench][invariance]") {
  CHECK_FALSE(Bench::Enabled());
  {
    const Tests::BenchSessionGuard session;
    CHECK(Bench::Enabled());
  }
  CHECK_FALSE(Bench::Enabled());
}

TEMPLATE_TEST_CASE_SIG(
    "A Bench session does not change the reads and hints of a FileReader",
    "[bench][invariance][file-reader]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const std::vector<BYTE> image = PatternImage(script_image_size);
  const bool keep_log = GENERATE(true, false);
  INFO("keep_log " << keep_log);

  const ScriptRun plain = RunScript<S>(image);
  CHECK(CountKind(plain.calls, Kind::Read) > 0);
  CHECK(CountKind(plain.calls, Kind::Prefetch) > 0);
  CHECK(CountKind(plain.calls, Kind::CancelPrefetch) == 2);
  CHECK(CountKind(plain.calls, Kind::PrefetchStanding) == 2);
  CHECK(CountKind(plain.calls, Kind::CancelStanding) == 2);

  Tests::BenchSessionGuard session(Bench::SessionOptions{.keep_log = keep_log});
  const ScriptRun measured = RunScript<S>(image);
  const Bench::Report report = session.Finish();

  CHECK(SameCallList(measured.calls, plain.calls));
  CHECK(measured.bytes == plain.bytes);
  CHECK(measured.succeeded == plain.succeeded);

  // The session did see the run it left alone.
  CHECK(report.requested_reads == CountKind(plain.calls, Kind::Read));
  CHECK(report.prefetch_calls == CountKind(plain.calls, Kind::Prefetch));
  CHECK(report.cancel_calls == CountKind(plain.calls, Kind::CancelPrefetch));
  CHECK(report.standing_prefetch_calls ==
        CountKind(plain.calls, Kind::PrefetchStanding));
  CHECK(report.standing_cancel_calls ==
        CountKind(plain.calls, Kind::CancelStanding));
  if (keep_log) {
    CHECK(
        SameCallList(LoggedReads(Bench::TakeReadLog()), ReadsOf(plain.calls)));
  } else {
    CHECK(Bench::TakeReadLog().events.empty());
  }
}

TEMPLATE_TEST_CASE_SIG(
    "A Bench session does not change the reads, log or tree of a scan",
    "[bench][invariance][tree]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const bool include_deleted = GENERATE(false, true);
  const unsigned threads = GENERATE(1U, 3U);
  const VolumeOptions options{.include_deleted = include_deleted,
                              .recover_errors = true};
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImageWithListMftScan(
      list_record_count, list_fragment_clusters);
  INFO("include_deleted " << include_deleted << ", threads " << threads);

  const Tests::ScanResult plain = ScanList<S>(image, options, threads);
  CHECK_FALSE(plain.tree.empty());
  CHECK_FALSE(plain.log.empty());
  CHECK(CountKind(plain.calls, Kind::Prefetch) > 0);

  Tests::BenchSessionGuard session;
  Tests::ScanResult measured = ScanList<S>(image, options, threads);
  const Bench::Report report = session.Finish();

  Tests::ScanResult unqueried = plain;
  const size_t plain_queries = TakeSizeQueries(unqueried.calls);
  CHECK(TakeSizeQueries(measured.calls) == plain_queries + 1);

  INFO("calls " << CallDivergence(unqueried.calls, measured.calls));
  Tests::CheckSameScan(unqueried, measured);
  CHECK(measured.parsed_by_threads == plain.parsed_by_threads);
  CHECK(measured.waiting_by_threads == plain.waiting_by_threads);
  CHECK(report.requested_reads == CountKind(plain.calls, Kind::Read));
  CHECK(report.prefetch_calls == CountKind(plain.calls, Kind::Prefetch));
  CHECK(report.cancel_calls == CountKind(plain.calls, Kind::CancelPrefetch));
  // The scan ran its phases inside the session.
  CHECK_FALSE(report.phases.empty());
}

TEMPLATE_TEST_CASE_SIG(
    "A Bench session does not change the reads or entries of a batched walk",
    "[bench][invariance][walk]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const VolumeOptions options{.include_deleted = true, .recover_errors = true};
  const std::vector<BYTE> image =
      Tests::BuildFakeNtfsImageWithIndexedMftScan(walk_record_count);

  WalkRun plain = WalkIndexed<S>(image, options);
  CHECK_FALSE(plain.walk.entries.empty());
  CHECK(CountKind(plain.calls, Kind::Prefetch) > 0);

  Tests::BenchSessionGuard session;
  WalkRun measured = WalkIndexed<S>(image, options);
  const Bench::Report report = session.Finish();
  const size_t plain_queries = TakeSizeQueries(plain.calls);
  CHECK(TakeSizeQueries(measured.calls) == plain_queries + 1);

  CHECK(measured.walk.entries == plain.walk.entries);
  CHECK(measured.walk.log == plain.walk.log);
  INFO("calls " << CallDivergence(plain.calls, measured.calls));
  CHECK(SameCallList(measured.calls, plain.calls));
  CHECK(report.requested_reads == CountKind(plain.calls, Kind::Read));
  CHECK(report.prefetch_calls == CountKind(plain.calls, Kind::Prefetch));
}
