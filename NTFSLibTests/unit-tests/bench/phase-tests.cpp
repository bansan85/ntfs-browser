#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/alloc/cluster-account-support.h>
#include <ntfs-browser/test/disk-readers/counting-disk-reader.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/test/support/bench-session-guard.h>
#include <ntfs-browser/test/support/test-log-sink.h>

#include "alloc/cluster-account.h"
#include "bench/bench.h"
#include "cache/strategy.h"
#include "disk-reader.h"
#include "io/file-reader.h"
#include "mft/tree.h"
#include "ntfs-volume.h"
#include "util/chunk-pool.h"
#include "volume-options.h"
#include "win-types.h"

namespace TestAlloc = NTFS_BROWSER_NAMESPACE::Test::Alloc;
namespace Alloc = NTFS_BROWSER_NAMESPACE::Alloc;
namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Mft = NTFS_BROWSER_NAMESPACE::Mft;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace TestDiskReaders = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestFakeImage = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Util = NTFS_BROWSER_NAMESPACE::Util;
using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::VolumeOptions;
using Kind = TestDiskReaders::CountingDiskReader::Kind;

namespace {

// Slots of the scan fixture: 3000 hold about 200 files with a list in a
// cluster of its own, so both passes of the scan read.
constexpr ULONGLONG list_record_count = 3000;
constexpr ULONGLONG list_fragment_clusters = 250;

// Bytes of the image the direct reads of these tests touch.
constexpr size_t small_image_size = 4 * 65536;

using Clock = std::chrono::steady_clock;

// Keeps the calling thread busy for at least duration, so that the time of a
// phase has a floor the scheduler cannot take away.
void SpinFor(std::chrono::microseconds duration) {
  const auto until = Clock::now() + duration;
  while (Clock::now() < until) {
    // Nothing to do: the wait is the point.
  }
}

const Bench::PhaseStat* FindPhase(const Bench::Report& report,
                                  Bench::Phase phase) {
  const auto found = std::ranges::find_if(
      report.phases,
      [phase](const Bench::PhaseStat& stat) { return stat.phase == phase; });
  return found == report.phases.end() ? nullptr : &*found;
}

const Bench::PhaseStat& RequirePhase(const Bench::Report& report,
                                     Bench::Phase phase) {
  const Bench::PhaseStat* const found = FindPhase(report, phase);
  INFO("phase " << Bench::PhaseName(phase));
  REQUIRE(found != nullptr);
  return *found;
}

size_t ThreadsWithRole(const Bench::Report& report, Bench::ThreadRole role) {
  return static_cast<size_t>(std::ranges::count_if(
      report.threads,
      [role](const Bench::ThreadStat& stat) { return stat.role == role; }));
}

// The phase markers of a log, in order, as the phase and whether it began.
std::vector<std::pair<Bench::Phase, bool>>
    PhaseMarkers(const Bench::ReadLog& log) {
  std::vector<std::pair<Bench::Phase, bool>> markers;
  for (const Bench::LogEvent& event : log.events) {
    if (event.type == Bench::EventType::PhaseBegin ||
        event.type == Bench::EventType::PhaseEnd) {
      markers.emplace_back(static_cast<Bench::Phase>(event.length),
                           event.type == Bench::EventType::PhaseBegin);
    }
  }
  return markers;
}

// What a scan inside a session left.
struct SessionScan {
  Bench::Report report;
  Bench::ReadLog log;
  Mft::MftTree::ScanStats stats;
  std::vector<TestDiskReaders::CountingDiskReader::Call> calls;
};

// Opens and scans the image over a counting reader, with a session when asked:
// the open is inside it, so the report holds the Open phase too.
template <Cache::Strategy S>
SessionScan ScanInSession(const std::vector<BYTE>& image,
                          const VolumeOptions& volume_options,
                          const Mft::MftTree::ScanOptions& scan_options,
                          bool with_session = true) {
  static_cast<void>(TestSupport::TakeCapturedLog());
  std::optional<TestSupport::BenchSessionGuard> session;
  if (with_session) {
    session.emplace();
  }
  SessionScan result;
  {
    auto reader = std::make_unique<TestDiskReaders::CountingDiskReader>(
        std::make_unique<TestDiskReaders::MemoryDiskReader>(image));
    const TestDiskReaders::CountingDiskReader* const counting = reader.get();
    const NtfsVolume<S> volume(std::move(reader), volume_options);
    REQUIRE(volume.IsVolumeOK());
    const Mft::MftTree tree(volume, scan_options);
    result.stats = tree.Stats();
    result.calls = counting->Calls();
  }
  if (session) {
    result.report = session->Finish();
    result.log = Bench::TakeReadLog();
  }
  return result;
}

size_t CountKind(
    const std::vector<TestDiskReaders::CountingDiskReader::Call>& calls,
    Kind kind) {
  return static_cast<size_t>(std::ranges::count_if(
      calls, [kind](const auto& call) { return call.kind == kind; }));
}

std::vector<BYTE> ListImage() {
  return TestFakeImage::BuildFakeNtfsImageWithListMftScan(
      list_record_count, list_fragment_clusters);
}

Mft::MftTree::ScanOptions OneThread() {
  Mft::MftTree::ScanOptions scan;
  scan.threads = 1;
  return scan;
}

}  // namespace

TEST_CASE("A phase scope nests, repeats and adds its repeats",
          "[bench][phase]") {
  TestSupport::BenchSessionGuard session;
  {
    const Bench::PhaseScope outer(Bench::Phase::Open);
    SpinFor(std::chrono::microseconds{500});
    {
      const Bench::PhaseScope inner(Bench::Phase::Pass1);
      SpinFor(std::chrono::microseconds{500});
    }
    {
      const Bench::PhaseScope inner(Bench::Phase::Pass1);
      SpinFor(std::chrono::microseconds{500});
    }
  }
  const Bench::Report report = session.Finish();
  const Bench::ReadLog log = Bench::TakeReadLog();

  REQUIRE(report.phases.size() == 2);
  const Bench::PhaseStat& open = RequirePhase(report, Bench::Phase::Open);
  const Bench::PhaseStat& pass1 = RequirePhase(report, Bench::Phase::Pass1);
  CHECK(open.count == 1);
  CHECK(pass1.count == 2);
  // The outer phase holds the inner ones, the inner ones add their repeats.
  CHECK(pass1.wall_ns >= 1000000);
  CHECK(open.wall_ns >= pass1.wall_ns + 500000);
  CHECK(open.first_begin_ns <= pass1.first_begin_ns);
  CHECK(pass1.last_end_ns <= open.last_end_ns);
  CHECK(pass1.first_begin_ns <= pass1.last_end_ns);
  CHECK(open.last_end_ns <= report.wall_ns);

  using Marker = std::pair<Bench::Phase, bool>;
  const std::vector<Marker> expected{
      {Bench::Phase::Open, true},   {Bench::Phase::Pass1, true},
      {Bench::Phase::Pass1, false}, {Bench::Phase::Pass1, true},
      {Bench::Phase::Pass1, false}, {Bench::Phase::Open, false}};
  CHECK(PhaseMarkers(log) == expected);
}

TEST_CASE("A read counts for the innermost phase only", "[bench][phase]") {
  const std::vector<BYTE> image(small_image_size, BYTE{1});
  auto reader = std::make_unique<TestDiskReaders::MemoryDiskReader>(image);
  const Io::FileReader<Cache::Strategy::NoCache> file(std::move(reader));
  std::vector<BYTE> buffer(100);
  LARGE_INTEGER addr{.QuadPart = 0};

  TestSupport::BenchSessionGuard session;
  REQUIRE(file.ReadInto(addr, buffer));
  {
    const Bench::PhaseScope outer(Bench::Phase::Open);
    addr.QuadPart = 4096;
    REQUIRE(file.ReadInto(addr, buffer));
    {
      const Bench::PhaseScope inner(Bench::Phase::Pass1);
      for (int i = 0; i < 3; i++) {
        addr.QuadPart = 8192 + (i * 4096);
        REQUIRE(file.ReadInto(addr, buffer));
      }
    }
  }
  const Bench::Report report = session.Finish();
  const Bench::ReadLog log = Bench::TakeReadLog();

  CHECK(report.requested_reads == 5);
  CHECK(RequirePhase(report, Bench::Phase::Open).reads == 1);
  CHECK(RequirePhase(report, Bench::Phase::Open).read_bytes == 100);
  CHECK(RequirePhase(report, Bench::Phase::Pass1).reads == 3);
  CHECK(RequirePhase(report, Bench::Phase::Pass1).read_bytes == 300);

  // Each read of the log carries the phase that was innermost.
  std::vector<std::uint8_t> phases;
  for (const Bench::LogEvent& event : log.events) {
    if (event.type == Bench::EventType::Read) {
      phases.push_back(event.phase);
    }
  }
  const auto open = static_cast<std::uint8_t>(Bench::Phase::Open);
  const auto pass1 = static_cast<std::uint8_t>(Bench::Phase::Pass1);
  const std::vector<std::uint8_t> expected{0, open, pass1, pass1, pass1};
  CHECK(phases == expected);
}

TEST_CASE("A phase scope outside a session records nothing", "[bench][phase]") {
  CHECK_FALSE(Bench::Enabled());
  {
    const Bench::PhaseScope before(Bench::Phase::Open);
    TestSupport::BenchSessionGuard session;
    {
      const Bench::PhaseScope inside(Bench::Phase::Link);
    }
    const Bench::Report report = session.Finish();
    // Only the scope that began inside the session counts.
    REQUIRE(report.phases.size() == 1);
    CHECK(report.phases.front().phase == Bench::Phase::Link);
  }
  CHECK_FALSE(Bench::Enabled());
}

TEST_CASE("Phases have names that parse back", "[bench][phase]") {
  for (std::size_t index = 0; index < Bench::phase_count; index++) {
    const auto phase = static_cast<Bench::Phase>(index);
    const std::optional<Bench::Phase> parsed =
        Bench::ParsePhase(Bench::PhaseName(phase));
    INFO("phase " << Bench::PhaseName(phase));
    REQUIRE(parsed.has_value());
    CHECK(*parsed == phase);
  }
  CHECK_FALSE(Bench::ParsePhase("no-such-phase").has_value());
  CHECK_FALSE(Bench::ParsePhase("").has_value());
}

TEMPLATE_TEST_CASE_SIG("A scan of a fake image reports its phases in order",
                       "[bench][phase][tree]", ((Cache::Strategy S), S),
                       Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  const unsigned threads = GENERATE(1U, 3U);
  INFO("threads " << threads);
  Mft::MftTree::ScanOptions scan;
  scan.threads = threads;

  const SessionScan run = ScanInSession<S>(
      ListImage(),
      VolumeOptions{.include_deleted = true, .recover_errors = true}, scan);

  CHECK(run.stats.complete);
  const Bench::Report& report = run.report;
  const Bench::PhaseStat& open = RequirePhase(report, Bench::Phase::Open);
  const Bench::PhaseStat& pass1 = RequirePhase(report, Bench::Phase::Pass1);
  const Bench::PhaseStat& pass2 = RequirePhase(report, Bench::Phase::Pass2);
  const Bench::PhaseStat& link = RequirePhase(report, Bench::Phase::Link);
  const Bench::PhaseStat& journal =
      RequirePhase(report, Bench::Phase::JournalLoad);

  // The tool's own phases are the tool's: a scan has none of them.
  CHECK(FindPhase(report, Bench::Phase::Print) == nullptr);
  CHECK(FindPhase(report, Bench::Phase::Walk) == nullptr);
  CHECK(FindPhase(report, Bench::Phase::Destroy) == nullptr);

  CHECK(pass1.count == 1);
  CHECK(pass2.count == 1);
  CHECK(link.count == 1);
  CHECK(journal.count == 1);
  CHECK(open.count >= 1);

  // The phases follow each other, and fit in the session.
  CHECK(open.last_end_ns <= pass1.first_begin_ns);
  CHECK(pass1.last_end_ns <= pass2.first_begin_ns);
  CHECK(pass2.last_end_ns <= link.first_begin_ns);
  CHECK(link.last_end_ns <= journal.first_begin_ns);
  CHECK(journal.last_end_ns <= report.wall_ns);
  for (const Bench::PhaseStat& stat : report.phases) {
    INFO("phase " << Bench::PhaseName(stat.phase));
    CHECK(stat.wall_ns <= report.wall_ns);
    CHECK(stat.first_begin_ns <= stat.last_end_ns);
    CHECK(stat.count > 0);
  }

  // Both passes read, and the reads sit in the phase that made them.
  CHECK(pass1.reads > 0);
  CHECK(pass1.read_bytes > 0);
  CHECK(pass2.reads > 0);
  CHECK(open.reads > 0);
  std::uint64_t phase_reads = 0;
  for (const Bench::PhaseStat& stat : report.phases) {
    phase_reads += stat.reads;
  }
  CHECK(phase_reads <= report.requested_reads);
  CHECK(report.requested_reads == CountKind(run.calls, Kind::Read));
  CHECK(report.wall_ns > 0);
}

TEST_CASE("A scan without a session reports no phase", "[bench][phase][tree]") {
  const SessionScan run = ScanInSession<Cache::Strategy::NoCache>(
      ListImage(), VolumeOptions{.include_deleted = true}, OneThread(),
      /*with_session=*/false);

  CHECK(run.stats.complete);
  CHECK(run.report.phases.empty());
  CHECK(run.report.requested_reads == 0);
  CHECK(run.log.events.empty());
  CHECK_FALSE(Bench::Enabled());
}

TEST_CASE("The cluster account times its flushes, sweep and content check",
          "[bench][phase][tree]") {
  constexpr ULONGLONG subject = 22;
  constexpr ULONGLONG first_cluster = 40;
  TestAlloc::Volume volume;
  volume.Put(subject,
             TestAlloc::Freed(
                 100, TestAlloc::Stream({{first_cluster, 4}},
                                        4 * TestFakeImage::fake_cluster_size)));
  // Data the content check finds neither empty nor metadata.
  for (size_t i = 0; i < 4 * TestFakeImage::fake_cluster_size; i++) {
    volume.image.at((first_cluster * TestFakeImage::fake_cluster_size) + i) =
        static_cast<BYTE>((i % 251) + 1);
  }

  const SessionScan run = ScanInSession<Cache::Strategy::NoCache>(
      volume.Build(), TestAlloc::DeletedAndRecover(),
      TestAlloc::Scan(Alloc::ClusterAccountLevel::CountsAndMap,
                      Alloc::DataCheckMode::Full));

  CHECK(run.stats.complete);
  const Bench::PhaseStat& pass2 = RequirePhase(run.report, Bench::Phase::Pass2);
  const Bench::PhaseStat& flush =
      RequirePhase(run.report, Bench::Phase::DataStateFlush);
  const Bench::PhaseStat& sweep =
      RequirePhase(run.report, Bench::Phase::ClusterSweep);
  const Bench::PhaseStat& check =
      RequirePhase(run.report, Bench::Phase::ContentCheck);
  const Bench::PhaseStat& link = RequirePhase(run.report, Bench::Phase::Link);
  // A flush sits inside the passes, the sweep after them, before the link.
  CHECK(flush.first_begin_ns <= pass2.last_end_ns);
  CHECK(pass2.last_end_ns <= sweep.first_begin_ns);
  CHECK(sweep.last_end_ns <= link.first_begin_ns);
  // The content check runs inside the sweep, and reads clusters.
  CHECK(sweep.first_begin_ns <= check.first_begin_ns);
  CHECK(check.last_end_ns <= sweep.last_end_ns);
  CHECK(sweep.wall_ns >= check.wall_ns);
  CHECK(check.reads > 0);
  CHECK(run.report.kinds.at(static_cast<size_t>(Bench::ReadKind::ContentCheck))
            .reads > 0);
}

TEMPLATE_TEST_CASE_SIG(
    "Stopping after the first pass leaves an incomplete tree and no second "
    "pass",
    "[bench][stop]", ((Cache::Strategy S), S), Cache::Strategy::NoCache,
    Cache::Strategy::FullCache) {
  const bool with_session = GENERATE(true, false);
  INFO("with_session " << with_session);
  const VolumeOptions volume_options{.include_deleted = true,
                                     .recover_errors = true};
  const std::vector<BYTE> image = ListImage();

  const SessionScan full =
      ScanInSession<S>(image, volume_options, OneThread(), with_session);
  CHECK(full.stats.complete);
  CHECK_FALSE(Bench::StopRequested());

  SessionScan stopped;
  {
    const TestSupport::StopAfterGuard stop(Bench::Phase::Pass1);
    CHECK_FALSE(Bench::StopRequested());
    stopped =
        ScanInSession<S>(image, volume_options, OneThread(), with_session);
    CHECK(Bench::StopRequested());
  }

  CHECK_FALSE(stopped.stats.complete);
  // The second pass reads the lists of the waiting records, and the full scan
  // does read them.
  const size_t full_reads = CountKind(full.calls, Kind::Read);
  const size_t stopped_reads = CountKind(stopped.calls, Kind::Read);
  CHECK(stopped_reads > 0);
  CHECK(stopped_reads < full_reads);
  if (with_session) {
    CHECK(full.report.kinds
              .at(static_cast<size_t>(Bench::ReadKind::AttributeList))
              .reads > 0);
    CHECK(stopped.report.kinds
              .at(static_cast<size_t>(Bench::ReadKind::AttributeList))
              .reads == 0);
    CHECK(RequirePhase(stopped.report, Bench::Phase::Pass1).count == 1);
    CHECK(FindPhase(stopped.report, Bench::Phase::Pass2) == nullptr);
    CHECK(FindPhase(stopped.report, Bench::Phase::Link) == nullptr);
    CHECK(FindPhase(stopped.report, Bench::Phase::JournalLoad) == nullptr);
  }
  // The guard took the request away.
  CHECK_FALSE(Bench::StopRequested());
}

TEST_CASE("Stopping after the second pass skips the link", "[bench][stop]") {
  SessionScan stopped;
  {
    const TestSupport::StopAfterGuard stop(Bench::Phase::Pass2);
    stopped = ScanInSession<Cache::Strategy::NoCache>(
        ListImage(), VolumeOptions{.include_deleted = true}, OneThread());
  }

  CHECK_FALSE(stopped.stats.complete);
  CHECK(RequirePhase(stopped.report, Bench::Phase::Pass1).count == 1);
  CHECK(RequirePhase(stopped.report, Bench::Phase::Pass2).count == 1);
  CHECK(FindPhase(stopped.report, Bench::Phase::Link) == nullptr);
}

TEST_CASE("Stopping after the link leaves an incomplete tree",
          "[bench][stop]") {
  SessionScan stopped;
  {
    const TestSupport::StopAfterGuard stop(Bench::Phase::Link);
    stopped = ScanInSession<Cache::Strategy::NoCache>(
        ListImage(), VolumeOptions{.include_deleted = true}, OneThread());
  }

  CHECK_FALSE(stopped.stats.complete);
  CHECK(RequirePhase(stopped.report, Bench::Phase::Link).count == 1);
}

TEST_CASE("Stopping after the cluster sweep leaves an incomplete tree",
          "[bench][stop]") {
  Mft::MftTree::ScanOptions scan = OneThread();
  scan.cluster_account = Alloc::ClusterAccountLevel::CountsAndMap;
  SessionScan stopped;
  {
    const TestSupport::StopAfterGuard stop(Bench::Phase::ClusterSweep);
    stopped = ScanInSession<Cache::Strategy::NoCache>(
        ListImage(), VolumeOptions{.include_deleted = true}, scan);
  }

  CHECK_FALSE(stopped.stats.complete);
  CHECK(RequirePhase(stopped.report, Bench::Phase::ClusterSweep).count == 1);
  CHECK(FindPhase(stopped.report, Bench::Phase::Link) == nullptr);
}

TEST_CASE("A new stop request clears the previous one", "[bench][stop]") {
  {
    const TestSupport::StopAfterGuard stop(Bench::Phase::Open);
    {
      const Bench::PhaseScope phase(Bench::Phase::Open);
    }
    CHECK(Bench::StopRequested());
    Bench::SetStopAfter(Bench::Phase::Link);
    CHECK_FALSE(Bench::StopRequested());
    {
      const Bench::PhaseScope other(Bench::Phase::Open);
    }
    CHECK_FALSE(Bench::StopRequested());
    {
      const Bench::PhaseScope phase(Bench::Phase::Link);
    }
    CHECK(Bench::StopRequested());
  }
  CHECK_FALSE(Bench::StopRequested());
}

TEST_CASE("A thread scope reports the thread when it ends", "[bench][thread]") {
  TestSupport::BenchSessionGuard session;
  std::thread worker([] {
    const Bench::ThreadScope scope(Bench::ThreadRole::OutputWriter);
    SpinFor(std::chrono::milliseconds{2});
  });
  worker.join();
  const Bench::Report report = session.Finish();

  REQUIRE(report.threads.size() == 2);
  CHECK(report.threads.front().role == Bench::ThreadRole::Main);
  const Bench::ThreadStat& writer = report.threads.back();
  CHECK(writer.role == Bench::ThreadRole::OutputWriter);
  CHECK(writer.wall_ns >= 2000000);
  CHECK(writer.wall_ns <= report.wall_ns);
}

TEST_CASE("A thread scope outside a session reports nothing",
          "[bench][thread]") {
  std::thread worker([] {
    const Bench::ThreadScope scope(Bench::ThreadRole::Other);
    SpinFor(std::chrono::milliseconds{1});
  });
  worker.join();

  TestSupport::BenchSessionGuard session;
  const Bench::Report report = session.Finish();

  REQUIRE(report.threads.size() == 1);
  CHECK(report.threads.front().role == Bench::ThreadRole::Main);
}

TEST_CASE("A chunk pool reports one thread stat per helper",
          "[bench][thread]") {
  constexpr size_t helpers = 3;
  TestSupport::BenchSessionGuard session;
  {
    Util::ChunkPool pool(helpers);
    CHECK(pool.Slots() == helpers + 1);
    pool.Start(8, [](size_t /*chunk*/, size_t /*slot*/) {
      SpinFor(std::chrono::microseconds{200});
    });
    pool.Finish();
  }
  const Bench::Report report = session.Finish();

  CHECK(ThreadsWithRole(report, Bench::ThreadRole::Main) == 1);
  CHECK(ThreadsWithRole(report, Bench::ThreadRole::PoolWorker) == helpers);
  CHECK(report.threads.size() == helpers + 1);
}

TEST_CASE("A pool without helpers reports no worker", "[bench][thread]") {
  TestSupport::BenchSessionGuard session;
  {
    Util::ChunkPool pool(0);
    pool.Start(2, [](size_t /*chunk*/, size_t /*slot*/) {});
    pool.WaitFor(1);
    pool.Finish();
  }
  const Bench::Report report = session.Finish();

  CHECK(ThreadsWithRole(report, Bench::ThreadRole::PoolWorker) == 0);
}

TEST_CASE("A scan with three threads reports its pool workers",
          "[bench][thread][tree]") {
  Mft::MftTree::ScanOptions scan;
  scan.threads = 3;
  const SessionScan one = ScanInSession<Cache::Strategy::NoCache>(
      ListImage(),
      VolumeOptions{.include_deleted = true, .recover_errors = true},
      OneThread());
  const SessionScan three = ScanInSession<Cache::Strategy::NoCache>(
      ListImage(),
      VolumeOptions{.include_deleted = true, .recover_errors = true}, scan);

  CHECK(ThreadsWithRole(one.report, Bench::ThreadRole::PoolWorker) == 0);
  CHECK(ThreadsWithRole(one.report, Bench::ThreadRole::Main) == 1);

  // Two helpers per pool: the calling thread is the third.
  const size_t workers =
      ThreadsWithRole(three.report, Bench::ThreadRole::PoolWorker);
  CHECK(workers >= 2);
  CHECK(workers % 2 == 0);
  CHECK(ThreadsWithRole(three.report, Bench::ThreadRole::Main) == 1);
  for (const Bench::ThreadStat& stat : three.report.threads) {
    CHECK(stat.wall_ns <= three.report.wall_ns);
  }
}
