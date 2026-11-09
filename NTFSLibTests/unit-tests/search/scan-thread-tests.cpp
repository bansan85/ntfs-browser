#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/scan.h>
#include <ntfs-browser/test/support/test-log-sink.h>
#include <ntfs-browser/types.h>

#include "log/ntfs-common.h"
#include "log/sink.h"
#include "scan-fake-engine.h"
#include "scan-support.h"
#include "search/engine.h"

namespace TestSearch = NTFS_BROWSER_NAMESPACE::Test::Search;
namespace TestSupport = NTFS_BROWSER_NAMESPACE::Test::Support;
namespace Lib = NTFS_BROWSER_NAMESPACE;
namespace Log = NTFS_BROWSER_NAMESPACE::Log;

namespace {

using TestSearch::AddNumbered;
using TestSearch::Gate;
using TestSearch::ScriptedEngineScope;

// Options for a scan that runs a scripted engine: the image only has to exist.
Lib::Options ScriptedOptions(const TestSearch::TempImage& image) {
  Lib::Options options;
  options.target = image.Target();
  return options;
}

// What a test callback keeps of the batches.
struct Collected {
  std::vector<std::uint64_t> records;
  std::vector<std::size_t> sizes;
  std::size_t calls{0};
  std::size_t last_calls{0};
  Lib::Progress progress;
};

void CollectInto(Lib::Scan& scan, Collected& collected) {
  scan.OnBatch([&collected](const Lib::Batch& batch) {
    ++collected.calls;
    collected.sizes.push_back(batch.entries.size());
    if (batch.last) {
      ++collected.last_calls;
    }
    collected.progress = batch.progress;
    for (const Lib::Entry& entry : batch.entries) {
      collected.records.push_back(entry.record);
    }
  });
}

// The numbers 0 to count - 1.
std::vector<std::uint64_t> Sequence(std::uint64_t count) {
  std::vector<std::uint64_t> numbers;
  for (std::uint64_t number = 0; number < count; ++number) {
    numbers.push_back(number);
  }
  return numbers;
}

// One line a sink received, with the thread that wrote it.
struct SeenLine {
  std::thread::id thread;
  Log::Level level;
  std::string text;
};

// Keeps every line with the id of the thread that called Write().
class ThreadSink final : public Log::ISink {
 public:
  [[nodiscard]] bool ShouldLog(Log::Level level) const override {
    return level != Log::Level::Off;
  }

  void Write(Log::Level level, std::string_view message) override {
    const std::scoped_lock lock(mutex_);
    lines_.push_back({.thread = std::this_thread::get_id(),
                      .level = level,
                      .text = std::string(message)});
  }

  void Flush() override {}

  [[nodiscard]] std::vector<SeenLine> Lines() {
    const std::scoped_lock lock(mutex_);
    return lines_;
  }

  [[nodiscard]] bool Has(std::string_view text) {
    const std::scoped_lock lock(mutex_);
    return std::ranges::any_of(
        lines_, [text](const SeenLine& line) { return line.text == text; });
  }

 private:
  std::mutex mutex_;
  std::vector<SeenLine> lines_;
};

// Makes sink the only sink of the library, and puts the capturing sink of the
// tests back afterwards.
class SinkScope final {
 public:
  explicit SinkScope(const std::shared_ptr<Log::ISink>& sink) {
    Log::SetSinks({sink});
  }

  SinkScope(const SinkScope&) = delete;
  SinkScope& operator=(const SinkScope&) = delete;
  SinkScope(SinkScope&&) = delete;
  SinkScope& operator=(SinkScope&&) = delete;

  ~SinkScope() { TestSupport::InstallCaptureSink(); }
};

// Waits until test() is true, at most a long time.
template <class Test>
bool WaitUntil(Test test) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!test()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

// Cancels scan after a long time, so that a Run() that fails to wake ends the
// test as Cancelled instead of hanging it.
class Watchdog final {
 public:
  explicit Watchdog(Lib::Scan& scan)
      : thread_([&scan](const std::stop_token& stop) {
          std::mutex mutex;
          std::condition_variable_any quiet;
          std::unique_lock lock(mutex);
          if (!quiet.wait_for(lock, stop, std::chrono::seconds(60),
                              [] { return false; }) &&
              !stop.stop_requested()) {
            scan.Cancel();
          }
        }) {}

  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;
  Watchdog(Watchdog&&) = delete;
  Watchdog& operator=(Watchdog&&) = delete;
  ~Watchdog() = default;

 private:
  std::jthread thread_;
};

}  // namespace

TEST_CASE("Start returns while the worker is held", "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const auto reached = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [gate, reached](Lib::EntrySink&, const std::stop_token& stop) {
        reached->Open();
        gate->Wait(stop);
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  Collected collected;
  CollectInto(scan, collected);

  scan.Start();
  REQUIRE(reached->WaitOpen());

  CHECK(scan.GetState() == Lib::State::Running);
  CHECK(scan.Poll() == Lib::State::Running);
  CHECK_FALSE(scan.Finished());
  CHECK(collected.calls == 0);

  gate->Open();
  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
  CHECK(collected.last_calls == 1);
}

TEST_CASE("Poll returns partial batches while the scan runs",
          "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const auto reached = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [gate, reached](Lib::EntrySink& sink, const std::stop_token& stop) {
        for (std::uint64_t number = 0; number < 3; ++number) {
          AddNumbered(sink, number);
        }
        reached->Open();
        gate->Wait(stop);
        for (std::uint64_t number = 3; number < 5; ++number) {
          AddNumbered(sink, number);
        }
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  Collected collected;
  CollectInto(scan, collected);

  scan.Start();
  REQUIRE(reached->WaitOpen());

  CHECK(scan.Poll() == Lib::State::Running);
  CHECK(collected.calls == 1);
  CHECK(collected.records == Sequence(3));
  CHECK(collected.last_calls == 0);
  CHECK(collected.progress.entries_found == 3);

  CHECK(scan.Poll() == Lib::State::Running);
  CHECK(collected.calls == 1);

  gate->Open();
  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
  CHECK(collected.records == Sequence(5));
  CHECK(collected.last_calls == 1);
  CHECK(collected.progress.entries_found == 5);
}

TEST_CASE("A progress change alone gives a batch with no entries",
          "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const auto reached = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [gate, reached](Lib::EntrySink& sink, const std::stop_token& stop) {
        sink.SetPercent(25.0);
        reached->Open();
        gate->Wait(stop);
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  Collected collected;
  CollectInto(scan, collected);

  scan.Start();
  REQUIRE(reached->WaitOpen());

  CHECK(scan.Poll() == Lib::State::Running);
  REQUIRE(collected.calls == 1);
  CHECK(collected.sizes.front() == 0);
  REQUIRE(collected.progress.percent.has_value());
  CHECK(*collected.progress.percent == 25.0);

  // Nothing is new now, so Poll() stays quiet however often it is called.
  for (int poll = 0; poll < 50; ++poll) {
    CHECK(scan.Poll() == Lib::State::Running);
  }
  CHECK(collected.calls == 1);

  gate->Open();
  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
}

TEST_CASE("No entry is lost or repeated while the caller polls hard",
          "[search][thread]") {
  const std::size_t max_pending =
      GENERATE(std::size_t{0}, std::size_t{7}, std::size_t{1000});
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
  Collected collected;
  CollectInto(scan, collected);

  scan.Start();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
  CHECK(collected.records == Sequence(count));
  CHECK(collected.last_calls == 1);
  CHECK(collected.progress.entries_found == count);
  if (max_pending != 0) {
    CHECK(*std::ranges::max_element(collected.sizes) <= max_pending);
  }
}

TEST_CASE("A slow caller holds the worker at the pending limit",
          "[search][thread]") {
  constexpr std::uint64_t limit = 10;
  constexpr std::uint64_t count = 100;
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto returned = std::make_shared<std::atomic<std::uint64_t>>(0);
  const ScriptedEngineScope engine(
      [returned](Lib::EntrySink& sink, const std::stop_token&) {
        for (std::uint64_t number = 0; number < count; ++number) {
          AddNumbered(sink, number);
          returned->fetch_add(1);
        }
        return true;
      });
  Lib::Options options = ScriptedOptions(image);
  options.max_pending_entries = limit;
  Lib::Scan scan(options);
  Collected collected;
  CollectInto(scan, collected);

  scan.Start();
  REQUIRE(WaitUntil([&] { return returned->load() >= limit; }));
  // The worker has had time to run past the limit if it were going to.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(returned->load() == limit);

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
  CHECK(collected.records == Sequence(count));
  CHECK(*std::ranges::max_element(collected.sizes) <= limit);
}

TEST_CASE("Cancel frees a worker held at the pending limit",
          "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto returned = std::make_shared<std::atomic<std::uint64_t>>(0);
  const auto done = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [returned, done](Lib::EntrySink& sink, const std::stop_token& stop) {
        for (std::uint64_t number = 0; number < 1000; ++number) {
          AddNumbered(sink, number);
          returned->fetch_add(1);
          if (stop.stop_requested()) {
            break;
          }
        }
        done->Open();
        return true;
      });
  Lib::Options options = ScriptedOptions(image);
  options.max_pending_entries = 1;
  Lib::Scan scan(options);

  scan.Start();
  REQUIRE(WaitUntil([&] { return returned->load() >= 1; }));
  scan.Cancel();

  CHECK(done->WaitOpen());
  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Cancelled);
}

TEST_CASE("Cancel during a held scan ends it as Cancelled",
          "[search][thread]") {
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
  Collected collected;
  CollectInto(scan, collected);

  scan.Start();
  REQUIRE(reached->WaitOpen());
  scan.Cancel();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Cancelled);
  CHECK(scan.Finished());
  CHECK(collected.records == Sequence(1));
  CHECK(collected.last_calls == 1);
}

TEST_CASE("The destructor of a running scan joins the worker",
          "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const auto reached = std::make_shared<Gate>();
  const auto exited = std::make_shared<std::atomic<bool>>(false);
  const ScriptedEngineScope engine(
      [gate, reached, exited](Lib::EntrySink&, const std::stop_token& stop) {
        reached->Open();
        gate->Wait(stop);
        // Slow to leave: a destructor that does not wait is seen here.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        exited->store(true);
        return true;
      });
  {
    Lib::Scan scan(ScriptedOptions(image));
    scan.Start();
    REQUIRE(reached->WaitOpen());
  }

  CHECK(exited->load());
  CHECK(engine.Destroyed().IsOpen());
}

TEST_CASE("An exception in the worker ends the scan as Failed",
          "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const ScriptedEngineScope engine(
      [](Lib::EntrySink& sink, const std::stop_token&) -> bool {
        AddNumbered(sink, 0);
        throw std::runtime_error("worker blew up");
      });
  static_cast<void>(TestSupport::TakeCapturedLog());
  Lib::Scan scan(ScriptedOptions(image));
  Collected collected;
  CollectInto(scan, collected);

  scan.Start();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Failed);
  CHECK(collected.records == Sequence(1));
  CHECK(collected.last_calls == 1);
  CHECK(TestSupport::TakeCapturedLog().find("worker blew up") !=
        std::string::npos);
}

TEST_CASE("An exception of an unknown type ends the scan as Failed",
          "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const ScriptedEngineScope engine(
      [](Lib::EntrySink&, const std::stop_token&) -> bool { throw 42; });
  Lib::Scan scan(ScriptedOptions(image));

  scan.Start();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Failed);
}

TEST_CASE("The log of the worker is replayed on the thread that polls",
          "[search][thread][log]") {
  constexpr std::uint64_t count = 3000;
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto worker = std::make_shared<std::thread::id>();
  const ScriptedEngineScope engine(
      [worker](Lib::EntrySink& sink, const std::stop_token&) {
        *worker = std::this_thread::get_id();
        for (std::uint64_t number = 0; number < count; ++number) {
          Log::Info("line {}", number);
          AddNumbered(sink, number);
        }
        Log::Warn("last line");
        return true;
      });
  const auto recorder = std::make_shared<ThreadSink>();
  const SinkScope sinks(recorder);
  Lib::Scan scan(ScriptedOptions(image));
  Collected collected;
  CollectInto(scan, collected);

  scan.Start();
  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);

  const std::vector<SeenLine> lines = recorder->Lines();
  REQUIRE(lines.size() == count + 1);
  for (std::uint64_t number = 0; number < count; ++number) {
    CHECK(lines.at(number).text == "line " + std::to_string(number));
    CHECK(lines.at(number).level == Log::Level::Info);
  }
  CHECK(lines.back().text == "last line");
  CHECK(lines.back().level == Log::Level::Warn);
  CHECK(*worker != std::this_thread::get_id());
  CHECK(std::ranges::all_of(lines, [](const SeenLine& line) {
    return line.thread == std::this_thread::get_id();
  }));
}

TEST_CASE("The log of a running worker reaches the sinks before the end",
          "[search][thread][log]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const auto reached = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [gate, reached](Lib::EntrySink& sink, const std::stop_token& stop) {
        Log::Info("early");
        AddNumbered(sink, 0);
        reached->Open();
        // Alive but slow: it reports now and then, which is where the log is
        // handed over.
        while (!stop.stop_requested() && !gate->IsOpen()) {
          sink.SetPercent(50.0);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
      });
  const auto recorder = std::make_shared<ThreadSink>();
  const SinkScope sinks(recorder);
  Lib::Scan scan(ScriptedOptions(image));

  scan.Start();
  REQUIRE(reached->WaitOpen());

  REQUIRE(WaitUntil([&] {
    static_cast<void>(scan.Poll());
    return recorder->Has("early");
  }));
  CHECK(scan.GetState() == Lib::State::Running);

  gate->Open();
  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
}

TEST_CASE("Nothing reaches the sinks from the worker thread",
          "[search][thread][log]") {
  const auto recorder = std::make_shared<ThreadSink>();
  const SinkScope sinks(recorder);
  Lib::Options options;
  options.target = u"/no/such/image/anywhere";
  Lib::Scan scan(options);

  scan.Start();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Failed);
  const std::vector<SeenLine> lines = recorder->Lines();
  REQUIRE_FALSE(lines.empty());
  CHECK(std::ranges::all_of(lines, [](const SeenLine& line) {
    return line.thread == std::this_thread::get_id();
  }));
}

TEST_CASE("The views of a batch are valid during the callback only",
          "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto gate = std::make_shared<Gate>();
  const auto reached = std::make_shared<Gate>();
  const ScriptedEngineScope engine(
      [gate, reached](Lib::EntrySink& sink, const std::stop_token& stop) {
        for (std::uint64_t number = 0; number < 3; ++number) {
          AddNumbered(sink, number);
        }
        reached->Open();
        gate->Wait(stop);
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));

  struct Kept {
    const char16_t* data;
    std::size_t size;
    std::u16string copy;
  };

  std::vector<Kept> kept;
  scan.OnBatch([&kept](const Lib::Batch& batch) {
    for (const Lib::Entry& entry : batch.entries) {
      kept.push_back(
          {entry.name.data(), entry.name.size(), std::u16string(entry.name)});
    }
  });

  scan.Start();
  REQUIRE(reached->WaitOpen());
  CHECK(scan.Poll() == Lib::State::Running);

  REQUIRE(kept.size() == 3);
  for (const Kept& view : kept) {
    CHECK(view.copy.starts_with(u"file"));
#ifndef NDEBUG
    // The scan still owns the memory. A debug build wrote the poison unit over
    // the text when the callback returned.
    for (std::size_t unit = 0; unit < view.size; ++unit) {
      CHECK(view.data[unit] == char16_t{0xFFFE});
    }
#endif
  }

  gate->Open();
  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
}

TEST_CASE("The buffers are recycled batch after batch", "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const auto step = std::make_shared<std::atomic<int>>(0);
  const ScriptedEngineScope engine(
      [step](Lib::EntrySink& sink, const std::stop_token& stop) {
        for (int round = 0; round < 5; ++round) {
          for (int number = 0; number < 4; ++number) {
            AddNumbered(sink, static_cast<std::uint64_t>(round * 4 + number));
          }
          step->store(round + 1);
          // The caller takes this round before the next one starts.
          while (!stop.stop_requested() && step->load() != -(round + 1)) {
            std::this_thread::yield();
          }
        }
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  Collected collected;
  CollectInto(scan, collected);

  scan.Start();
  for (int round = 0; round < 5; ++round) {
    REQUIRE(WaitUntil([&] { return step->load() == round + 1; }));
    CHECK(scan.Poll() == Lib::State::Running);
    step->store(-(round + 1));
  }

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
  CHECK(collected.records == Sequence(20));
  CHECK(collected.sizes.front() == 4);
}

TEST_CASE("A Poll from the callback does nothing", "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const ScriptedEngineScope engine(
      [](Lib::EntrySink& sink, const std::stop_token&) {
        AddNumbered(sink, 0);
        return true;
      });
  Lib::Scan scan(ScriptedOptions(image));
  std::size_t calls = 0;
  scan.OnBatch([&](const Lib::Batch&) {
    ++calls;
    static_cast<void>(scan.Poll());
  });

  scan.Start();

  CHECK(TestSearch::PollToEnd(scan) == Lib::State::Finished);
  CHECK(calls >= 1);
  CHECK(calls <= 2);
}

TEST_CASE("The directory and MftList engines agree whatever the pace",
          "[search][thread]") {
  const TestSearch::TempImage image(TestSearch::TreeImage());
  const Lib::EngineKind kind =
      GENERATE(Lib::EngineKind::DirectoryNoCache,
               Lib::EngineKind::DirectoryFullCache, Lib::EngineKind::MftList);
  Lib::Options options;
  options.target = image.Target();
  options.engine = kind;
  options.recursive = true;
  options.include_deleted = true;

  const auto reference = TestSearch::RunScan(options);
  REQUIRE(reference.state == Lib::State::Finished);

  SECTION("a worker held back at one entry") {
    Lib::Options tight = options;
    tight.max_pending_entries = 1;
    const auto outcome = TestSearch::RunScan(tight);
    CHECK(outcome.state == Lib::State::Finished);
    auto rows = outcome.rows;
    auto expected = reference.rows;
    const auto order = [](const TestSearch::Row& left,
                          const TestSearch::Row& right) {
      return std::tie(left.record, left.parent_record, left.name) <
             std::tie(right.record, right.parent_record, right.name);
    };
    std::ranges::sort(rows, order);
    std::ranges::sort(expected, order);
    CHECK(rows == expected);
  }

  SECTION("a caller that polls slowly") {
    Lib::Scan scan(options);
    std::size_t entries = 0;
    scan.OnBatch([&entries](const Lib::Batch& batch) {
      entries += batch.entries.size();
    });
    scan.Start();
    while (!scan.Finished()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      static_cast<void>(scan.Poll());
    }
    CHECK(scan.GetState() == Lib::State::Finished);
    CHECK(entries == reference.rows.size());
  }
}

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
