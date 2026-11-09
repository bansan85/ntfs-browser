#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/example/ntfsscanbench/via-scan.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/disk-readers/throttled-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/types.h>

#include "disk-reader.h"
#include "scan-support.h"
#include "search/scan-probe.h"
#include "search/target.h"
#include "win-types.h"

namespace Fake = NTFS_BROWSER_NAMESPACE::Test::FakeImage;
namespace Lib = NTFS_BROWSER_NAMESPACE;
namespace TestDisk = NTFS_BROWSER_NAMESPACE::Test::DiskReaders;
namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace Bench = NTFS_BROWSER_NAMESPACE::Example::NtfsScanBench;
using TestSearch::TempImage;

namespace {

// Directories of the tree, files in each, and the length of the file names. A
// run of 28 'a' is what makes the heavy regex backtrack for a while without
// reaching the match limit.
constexpr std::size_t tree_directories = 8;
constexpr std::size_t files_per_directory = 12;
constexpr std::size_t name_run = 28;

// A tree of directories that hold many files with long names of 'a'.
std::vector<BYTE> BigTreeImage() {
  using Fake::FakeTreeNode;
  std::vector<FakeTreeNode> nodes;
  for (std::size_t dir = 0; dir < tree_directories; ++dir) {
    nodes.push_back({.links = {{.name = L"dir" + std::to_wstring(dir)}},
                     .directory = true});
  }
  for (std::size_t dir = 0; dir < tree_directories; ++dir) {
    for (std::size_t file = 0; file < files_per_directory; ++file) {
      nodes.push_back({.links = {{.name = std::wstring(name_run, L'a') +
                                          std::to_wstring(file) + L".txt",
                                  .parent = dir}},
                       .data_size = 10});
    }
  }
  return Fake::BuildFakeNtfsImageWithNameTree(nodes);
}

// Entries of BigTreeImage(): its directories and its files.
constexpr std::size_t big_tree_entries =
    tree_directories + (tree_directories * files_per_directory);

Lib::Options ScanOf(const TempImage& image, Lib::EngineKind engine) {
  Lib::Options options;
  options.target = image.Target();
  options.engine = engine;
  options.recursive = true;
  return options;
}

// Makes every scan read through a simulated disk over image, and puts the real
// readers back when it goes out of scope. The clock is the disk's.
class SimulatedDisk final {
 public:
  SimulatedDisk(const std::vector<BYTE>& image,
                const TestDisk::DiskModel& model) {
    Lib::SetReaderFactoryForTests(
        [&image, model, this](const Lib::TargetParts&)
            -> std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader> {
          ++opened_;
          return std::make_unique<TestDisk::ThrottledDiskReader>(
              std::make_unique<TestDisk::MemoryDiskReader>(image), model,
              clock_);
        });
  }

  SimulatedDisk(const SimulatedDisk&) = delete;
  SimulatedDisk& operator=(const SimulatedDisk&) = delete;
  SimulatedDisk(SimulatedDisk&&) = delete;
  SimulatedDisk& operator=(SimulatedDisk&&) = delete;

  ~SimulatedDisk() { Lib::SetReaderFactoryForTests({}); }

  // Simulated nanoseconds the disk took.
  [[nodiscard]] std::uint64_t Ns() const noexcept { return clock_.NowNs(); }

  // Times a scan asked for a reader.
  [[nodiscard]] unsigned Opened() const noexcept { return opened_; }

 private:
  TestDisk::SimulatedClock clock_;
  unsigned opened_{0};
};

// A disk that costs nothing: no seek, no fixed cost, a bandwidth no scan uses
// up.
TestDisk::DiskModel FreeDisk() {
  constexpr std::uint64_t huge = std::uint64_t{1} << 50U;
  TestDisk::DiskModel model;
  model.seek_ns = 0;
  model.per_read_ns = 0;
  model.single_bytes_per_second = huge;
  model.max_bytes_per_second = huge;
  model.lanes = 8;
  return model;
}

// A disk that is slow on every read: 400 ms to reach a position, one lane. The
// time is simulated, so it costs nothing, and it dwarfs the CPU of any scan.
TestDisk::DiskModel SlowDisk() {
  constexpr std::uint64_t seek_ns = 400'000'000;
  constexpr std::uint64_t per_read_ns = 1000000;
  constexpr std::uint64_t bytes_per_second = 100'000'000;
  TestDisk::DiskModel model;
  model.seek_ns = seek_ns;
  model.per_read_ns = per_read_ns;
  model.single_bytes_per_second = bytes_per_second;
  model.max_bytes_per_second = bytes_per_second;
  model.lanes = 1;
  return model;
}

}  // namespace

TEST_CASE("A probed scan counts the entries its worker saw and delivered",
          "[search][probe]") {
  const TempImage image(BigTreeImage());
  Lib::Options options = ScanOf(image, Lib::EngineKind::DirectoryFullCache);
  options.pattern.kind = Lib::PatternKind::Wildcard;
  options.pattern.text = u"*5.txt";

  const Bench::ScanMeasure measure = Bench::MeasureScan(options);

  REQUIRE(measure.state == Lib::State::Finished);
  CHECK(measure.worker.entries_seen >= big_tree_entries);
  CHECK(measure.worker.entries_delivered > 0);
  CHECK(measure.worker.entries_delivered < measure.worker.entries_seen);
  CHECK(measure.worker.entries_delivered == measure.entries);
  CHECK(measure.worker.match_ns > 0);
  CHECK(measure.worker.exchange_ns > 0);
  CHECK(measure.worker.wall_ns >=
        measure.worker.match_ns + measure.worker.exchange_ns);
  CHECK(Lib::ScanProbeTarget() == nullptr);
}

TEST_CASE("A scan without a pattern delivers every entry it saw",
          "[search][probe]") {
  const TempImage image(BigTreeImage());

  const Bench::ScanMeasure measure =
      Bench::MeasureScan(ScanOf(image, Lib::EngineKind::MftList));

  REQUIRE(measure.state == Lib::State::Finished);
  CHECK(measure.worker.entries_seen == measure.worker.entries_delivered);
  CHECK(measure.entries == measure.worker.entries_delivered);
}

TEST_CASE("A probe does not change what a scan gives", "[search][probe]") {
  const TempImage image(BigTreeImage());
  for (const Lib::EngineKind engine :
       {Lib::EngineKind::MftList, Lib::EngineKind::DirectoryNoCache,
        Lib::EngineKind::DirectoryFullCache}) {
    const Lib::Options options = ScanOf(image, engine);
    const TestSearch::Outcome plain = TestSearch::RunScan(options);

    Lib::ScanTimings timings;
    Lib::SetScanProbeForTests(&timings);
    const TestSearch::Outcome probed = TestSearch::RunScan(options);
    Lib::SetScanProbeForTests(nullptr);

    REQUIRE(plain.state == Lib::State::Finished);
    CHECK(probed.state == plain.state);
    CHECK(probed.rows == plain.rows);
    CHECK(timings.entries_delivered == plain.rows.size());
  }
}

TEST_CASE("A scan with a small pending buffer reports the wait of its worker",
          "[search][probe]") {
  const TempImage image(BigTreeImage());
  Lib::Options options = ScanOf(image, Lib::EngineKind::DirectoryFullCache);
  options.max_pending_entries = 1;

  const Bench::ScanMeasure measure = Bench::MeasureScan(options);

  REQUIRE(measure.state == Lib::State::Finished);
  CHECK(measure.entries == measure.worker.entries_delivered);
  CHECK(measure.worker.wait_ns > 0);
}

TEST_CASE("A scan reads through the reader of the factory", "[search][probe]") {
  const std::vector<BYTE> bytes = BigTreeImage();
  const TempImage image(bytes);
  const Bench::ScanMeasure real =
      Bench::MeasureScan(ScanOf(image, Lib::EngineKind::DirectoryFullCache));

  const SimulatedDisk disk(bytes, FreeDisk());
  const Bench::ScanMeasure simulated =
      Bench::MeasureScan(ScanOf(image, Lib::EngineKind::DirectoryFullCache));

  REQUIRE(simulated.state == Lib::State::Finished);
  CHECK(disk.Opened() == 1);
  CHECK(simulated.entries == real.entries);
}

TEST_CASE("On a slow simulated disk the I/O is the larger floor",
          "[search][probe]") {
  const std::vector<BYTE> bytes = BigTreeImage();
  const TempImage image(bytes);
  const SimulatedDisk disk(bytes, SlowDisk());

  const Bench::ScanMeasure measure =
      Bench::MeasureScan(ScanOf(image, Lib::EngineKind::DirectoryFullCache));

  REQUIRE(measure.state == Lib::State::Finished);
  Bench::ScanFigures figures;
  figures.io_ns = disk.Ns();
  figures.cpu_ns = measure.wall_ns;
  CHECK(figures.io_ns > figures.cpu_ns);
  // The run that hides the CPU under the disk takes as long as the disk.
  figures.wall_ns = std::max(figures.io_ns, figures.cpu_ns);
  const Bench::Verdict verdict = Bench::Judge(figures);
  CHECK((verdict.path == Bench::CriticalPath::Io ||
         verdict.path == Bench::CriticalPath::IoStrong));
}

TEST_CASE("With a heavy regex on a free disk the CPU is the larger floor",
          "[search][probe]") {
  const std::vector<BYTE> bytes = BigTreeImage();
  const TempImage image(bytes);
  const SimulatedDisk disk(bytes, FreeDisk());
  Lib::Options options = ScanOf(image, Lib::EngineKind::DirectoryFullCache);
  options.pattern.kind = Lib::PatternKind::Regex;
  options.pattern.text = u"^(a|aa)+$";

  const Bench::ScanMeasure measure = Bench::MeasureScan(options);

  if (!Lib::RegexAvailable()) {
    CHECK(measure.state == Lib::State::Failed);
    return;
  }
  REQUIRE(measure.state == Lib::State::Finished);
  Bench::ScanFigures figures;
  // A disk that costs nothing may read 0 ns: Judge() takes that as unknown.
  figures.io_ns = std::max<std::uint64_t>(1, disk.Ns());
  figures.cpu_ns = measure.wall_ns;
  CHECK(figures.cpu_ns > figures.io_ns);
  CHECK(measure.worker.match_ns > measure.worker.exchange_ns);
  // Most of the worker's time goes to the pattern, so the stage is serial CPU.
  CHECK(measure.worker.match_ns * 2 > measure.worker.wall_ns);
  figures.wall_ns = std::max(figures.io_ns, figures.cpu_ns);
  CHECK(Bench::Judge(figures).path == Bench::CriticalPath::Cpu);
}
