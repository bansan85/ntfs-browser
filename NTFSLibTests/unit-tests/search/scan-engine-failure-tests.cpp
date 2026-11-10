#include <filesystem>
#include <memory>
#include <stdexcept>
#include <stop_token>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/types.h>

#include "disk-reader.h"
#include "scan-fake-engine.h"
#include "scan-support.h"
#include "scan-thread-support.h"
#include "search/engine.h"
#include "search/scan-probe.h"
#include "search/target.h"
#include "win-types.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace Lib = NTFS_BROWSER_NAMESPACE;
using TestSearch::ScriptedEngineScope;
using TestSearch::TempImage;

namespace {

// Puts the real engines back when it goes out of scope.
class EngineFactoryGuard final {
 public:
  explicit EngineFactoryGuard(Lib::EngineFactory factory) {
    Lib::SetEngineFactoryForTests(std::move(factory));
  }

  EngineFactoryGuard(const EngineFactoryGuard&) = delete;
  EngineFactoryGuard& operator=(const EngineFactoryGuard&) = delete;
  EngineFactoryGuard(EngineFactoryGuard&&) = delete;
  EngineFactoryGuard& operator=(EngineFactoryGuard&&) = delete;

  ~EngineFactoryGuard() { Lib::SetEngineFactoryForTests({}); }
};

// Puts the real readers back when it goes out of scope.
class ReaderFactoryGuard final {
 public:
  explicit ReaderFactoryGuard(Lib::ReaderFactory factory) {
    Lib::SetReaderFactoryForTests(std::move(factory));
  }

  ReaderFactoryGuard(const ReaderFactoryGuard&) = delete;
  ReaderFactoryGuard& operator=(const ReaderFactoryGuard&) = delete;
  ReaderFactoryGuard(ReaderFactoryGuard&&) = delete;
  ReaderFactoryGuard& operator=(ReaderFactoryGuard&&) = delete;

  ~ReaderFactoryGuard() { Lib::SetReaderFactoryForTests({}); }
};

// Takes the timing probe away when it goes out of scope.
class ProbeGuard final {
 public:
  explicit ProbeGuard(Lib::ScanTimings& timings) {
    Lib::SetScanProbeForTests(&timings);
  }

  ProbeGuard(const ProbeGuard&) = delete;
  ProbeGuard& operator=(const ProbeGuard&) = delete;
  ProbeGuard(ProbeGuard&&) = delete;
  ProbeGuard& operator=(ProbeGuard&&) = delete;

  ~ProbeGuard() { Lib::SetScanProbeForTests(nullptr); }
};

}  // namespace

TEST_CASE("A scan whose engine cannot be made fails", "[search][scan]") {
  const TempImage image(TestSearch::TreeImage());
  const EngineFactoryGuard factory(
      [](const Lib::Options&,
         const Lib::TargetParts&) -> std::unique_ptr<Lib::Engine> {
        return nullptr;
      });
  Lib::Scan scan(TestSearch::ScriptedOptions(image));

  scan.Start();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Failed);
}

TEST_CASE("A scan whose reader cannot be made fails", "[search][scan]") {
  const TempImage image(TestSearch::TreeImage());
  const ReaderFactoryGuard factory(
      [](const Lib::TargetParts&) -> std::unique_ptr<Lib::IDiskReader> {
        return nullptr;
      });
  Lib::Options options;
  options.target = image.Target();
  options.engine = Lib::EngineKind::DirectoryFullCache;

  const auto outcome = TestSearch::RunScan(options);

  CHECK(outcome.state == Lib::State::Failed);
  CHECK(outcome.rows.empty());
}

TEST_CASE("A timed scan whose engine throws still reports its timings",
          "[search][scan]") {
  const TempImage image(TestSearch::TreeImage());
  const ScriptedEngineScope engine(
      [](Lib::EntrySink&, const std::stop_token&) -> bool {
        throw std::runtime_error("the engine broke");
      });
  Lib::ScanTimings timings;
  const ProbeGuard probe(timings);
  Lib::Scan scan(TestSearch::ScriptedOptions(image));

  scan.Start();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Failed);
  CHECK(timings.worker_wall_ns > 0);
}

#ifdef _WIN32

namespace {

// Holds a file open with no sharing until it goes out of scope.
class ExclusiveHandle final {
 public:
  explicit ExclusiveHandle(const std::filesystem::path& path)
      : handle_(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}

  ExclusiveHandle(const ExclusiveHandle&) = delete;
  ExclusiveHandle& operator=(const ExclusiveHandle&) = delete;
  ExclusiveHandle(ExclusiveHandle&&) = delete;
  ExclusiveHandle& operator=(ExclusiveHandle&&) = delete;

  ~ExclusiveHandle() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }

  [[nodiscard]] bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE handle_;
};

}  // namespace

TEST_CASE("A scan of an image that another process holds fails",
          "[search][scan]") {
  const TempImage image(TestSearch::TreeImage());
  const ExclusiveHandle lock(image.Path());
  REQUIRE(lock.IsOpen());
  Lib::Options options;
  options.target = image.Target();
  options.engine = Lib::EngineKind::DirectoryFullCache;

  const auto outcome = TestSearch::RunScan(options);

  CHECK(outcome.state == Lib::State::Failed);
}

#endif  // _WIN32
