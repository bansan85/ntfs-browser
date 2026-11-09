#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/types.h>

#include "log/ntfs-common.h"
#include "scan-fake-engine.h"
#include "scan-support.h"
#include "scan-thread-support.h"
#include "search/engine.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace Lib = NTFS_BROWSER_NAMESPACE;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;

namespace {

using TestSearch::AddNumbered;
using TestSearch::Collected;
using TestSearch::CollectInto;
using TestSearch::Gate;
using TestSearch::ScriptedEngineScope;
using TestSearch::ScriptedOptions;
using TestSearch::SeenLine;
using TestSearch::Sequence;
using TestSearch::SinkScope;
using TestSearch::ThreadSink;
using TestSearch::Watchdog;

}  // namespace

TEST_CASE("Run delivers every entry once and returns Finished",
          "[search][thread][run]") {
  const std::size_t max_pending =
      GENERATE(std::size_t{0}, std::size_t{7}, std::size_t{1000});
  const std::chrono::milliseconds interval =
      GENERATE(std::chrono::milliseconds(1), std::chrono::milliseconds(100));
  constexpr std::uint64_t count = 20000;
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const ScriptedEngineScope engine(
      [](Lib::EntrySink& sink, const std::stop_token&) {
        for (std::uint64_t number = 0; number < count; ++number) {
          AddNumbered(sink, number);
          if (number % 1000 == 0) {
            sink.SetPercent(100.0 * static_cast<double>(number) /
                            static_cast<double>(count));
          }
        }
        return true;
      });
  Lib::Options options = ScriptedOptions(image);
  options.max_pending_entries = max_pending;
  Lib::Scan scan(options);
  const Watchdog watchdog(scan);
  Collected collected;
  CollectInto(scan, collected);

  CHECK(scan.Run(interval) == Lib::State::Finished);

  CHECK(scan.Finished());
  CHECK(collected.records == Sequence(count));
  CHECK(collected.last_calls == 1);
  CHECK(collected.progress.entries_found == count);
}

TEST_CASE("Run wakes early on news and keeps the batches apart",
          "[search][thread][run]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [gate](Lib::EntrySink& sink, const std::stop_token& stop) {
        AddNumbered(sink, 0);
        gate->Wait(stop);
        AddNumbered(sink, 1);
        AddNumbered(sink, 2);
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  const Watchdog watchdog(scan);
  Collected collected;
  scan.OnBatch([&](const Lib::Batch& batch) {
    ++collected.calls;
    collected.sizes.push_back(batch.entries.size());
    for (const Lib::Entry& entry : batch.entries) {
      collected.records.push_back(entry.record);
    }
    gate->Open();
  });

  // The first entry wakes Run() long before the hour is over. The two that
  // follow do not: one interval has not passed since that batch, so they come
  // with the end.
  CHECK(scan.Run(std::chrono::hours(1)) == Lib::State::Finished);

  CHECK(collected.records == Sequence(3));
  CHECK(collected.sizes == std::vector<std::size_t>{1, 2});
}

TEST_CASE("Run returns promptly when the scan ends", "[search][thread][run]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const ScriptedEngineScope engine(
      [](Lib::EntrySink&, const std::stop_token&) { return true; });
  Lib::Scan scan(ScriptedOptions(image));
  const Watchdog watchdog(scan);
  Collected collected;
  CollectInto(scan, collected);

  CHECK(scan.Run(std::chrono::hours(1)) == Lib::State::Finished);

  CHECK(collected.last_calls == 1);
  CHECK(collected.records.empty());
}

TEST_CASE("Cancel from the callback ends Run as Cancelled",
          "[search][thread][run]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [gate](Lib::EntrySink& sink, const std::stop_token& stop) {
        AddNumbered(sink, 0);
        gate->Wait(stop);
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  const Watchdog watchdog(scan);
  Collected collected;
  scan.OnBatch([&](const Lib::Batch& batch) {
    ++collected.calls;
    if (batch.last) {
      ++collected.last_calls;
    }
    for (const Lib::Entry& entry : batch.entries) {
      collected.records.push_back(entry.record);
    }
    scan.Cancel();
  });

  CHECK(scan.Run(std::chrono::hours(1)) == Lib::State::Cancelled);

  CHECK(collected.records == Sequence(1));
  CHECK(collected.last_calls == 1);
}

TEST_CASE("Cancel from another thread wakes a Run that waits",
          "[search][thread][run]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const auto reached = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [gate, reached](Lib::EntrySink& sink, const std::stop_token& stop) {
        AddNumbered(sink, 0);
        reached->Open();
        gate->Wait(stop);
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  const Watchdog watchdog(scan);
  Collected collected;
  CollectInto(scan, collected);

  std::thread canceller([&] {
    if (reached->WaitOpen()) {
      scan.Cancel();
    }
  });
  const Lib::State state = scan.Run(std::chrono::hours(1));
  canceller.join();

  CHECK(state == Lib::State::Cancelled);
  CHECK(collected.records == Sequence(1));
  CHECK(collected.last_calls == 1);
}

TEST_CASE("Run on a target that does not open returns Failed with the log",
          "[search][thread][run][log]") {
  const auto recorder = std::make_shared<ThreadSink>();
  const SinkScope sinks(recorder);
  Lib::Options options;
  options.target = u"/no/such/image/anywhere";
  Lib::Scan scan(options);
  const Watchdog watchdog(scan);
  Collected collected;
  CollectInto(scan, collected);

  CHECK(scan.Run(std::chrono::hours(1)) == Lib::State::Failed);

  CHECK(collected.last_calls == 1);
  const std::vector<SeenLine> lines = recorder->Lines();
  REQUIRE_FALSE(lines.empty());
  CHECK(std::ranges::any_of(lines, [](const SeenLine& line) {
    return line.level == Log::Level::Error;
  }));
  CHECK(std::ranges::all_of(lines, [](const SeenLine& line) {
    return line.thread == std::this_thread::get_id();
  }));
}

TEST_CASE("Run called twice, or after Start, runs the scan once",
          "[search][thread][run]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto starts = std::make_shared<std::atomic<int>>(0);
  const ScriptedEngineScope engine(
      [starts](Lib::EntrySink& sink, const std::stop_token&) {
        starts->fetch_add(1);
        for (std::uint64_t number = 0; number < 3; ++number) {
          AddNumbered(sink, number);
        }
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  const Watchdog watchdog(scan);
  Collected collected;
  CollectInto(scan, collected);

  const bool after_start = GENERATE(false, true);
  if (after_start) {
    scan.Start();
  }
  CHECK(scan.Run(std::chrono::milliseconds(10)) == Lib::State::Finished);
  const std::size_t calls = collected.calls;
  CHECK(scan.Run(std::chrono::hours(1)) == Lib::State::Finished);

  CHECK(collected.calls == calls);
  CHECK(collected.records == Sequence(3));
  CHECK(starts->load() == 1);
}

TEST_CASE("Run from the callback does nothing", "[search][thread][run]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [gate](Lib::EntrySink& sink, const std::stop_token& stop) {
        AddNumbered(sink, 0);
        gate->Wait(stop);
        AddNumbered(sink, 1);
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  const Watchdog watchdog(scan);
  std::vector<std::pair<bool, Lib::State>> inner;
  scan.OnBatch([&](const Lib::Batch& batch) {
    inner.emplace_back(batch.last, scan.Run(std::chrono::hours(1)));
    gate->Open();
  });

  CHECK(scan.Run(std::chrono::hours(1)) == Lib::State::Finished);

  REQUIRE(inner.size() == 2);
  CHECK(inner.front() == std::pair{false, Lib::State::Running});
  CHECK(inner.back() == std::pair{true, Lib::State::Finished});
}

TEST_CASE("Run with an interval of zero polls after each wake",
          "[search][thread][run]") {
  const std::chrono::milliseconds interval =
      GENERATE(std::chrono::milliseconds(0), std::chrono::milliseconds(-5));
  constexpr std::uint64_t count = 2000;
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const ScriptedEngineScope engine(
      [](Lib::EntrySink& sink, const std::stop_token&) {
        for (std::uint64_t number = 0; number < count; ++number) {
          AddNumbered(sink, number);
        }
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  const Watchdog watchdog(scan);
  Collected collected;
  CollectInto(scan, collected);

  CHECK(scan.Run(interval) == Lib::State::Finished);

  CHECK(collected.records == Sequence(count));
  CHECK(collected.last_calls == 1);
}
