#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "ordered-chunks.h"

using NtfsDevice::ChunkJob;
using NtfsDevice::ChunkSteps;
using NtfsDevice::RunOrderedChunks;

namespace {

// What a run of RunOrderedChunks() did, as the steps noted it.
struct Trace {
  explicit Trace(size_t chunk_count)
      : prepared(chunk_count, 0), made(chunk_count), slot_busy(64) {}

  std::vector<size_t> prepare_order;
  std::vector<size_t> consume_order;
  // Written by the calling thread alone.
  std::vector<char> prepared;
  // Chunks made, by any thread.
  std::vector<std::atomic<int>> made;
  std::vector<std::atomic<int>> slot_busy;
  std::atomic<bool> made_unprepared{false};
  std::atomic<bool> made_unprepared_consumed{false};
  std::atomic<bool> shared_slot{false};
  std::atomic<size_t> highest_slot{0};
  size_t held = 0;
  size_t most_held = 0;
  std::thread::id caller = std::this_thread::get_id();
  bool step_off_caller = false;
};

// Steps that note what they see in trace. make() takes a moment, so that
// threads overlap.
ChunkSteps TracingSteps(Trace& trace) {
  ChunkSteps steps;
  steps.prepare = [&trace](size_t chunk) {
    trace.step_off_caller =
        trace.step_off_caller || std::this_thread::get_id() != trace.caller;
    trace.prepare_order.push_back(chunk);
    trace.prepared[chunk] = 1;
    trace.held++;
    trace.most_held = (std::max)(trace.most_held, trace.held);
  };
  steps.make = [&trace](size_t chunk, size_t slot) {
    // prepared[chunk] was written before the chunk was handed out.
    if (trace.prepared[chunk] == 0) {
      trace.made_unprepared = true;
    }
    if (trace.slot_busy[slot].fetch_add(1) != 0) {
      trace.shared_slot = true;
    }
    trace.highest_slot = (std::max)(trace.highest_slot.load(), slot);
    std::this_thread::yield();
    trace.made[chunk]++;
    trace.slot_busy[slot]--;
  };
  steps.consume = [&trace](size_t chunk) {
    trace.step_off_caller =
        trace.step_off_caller || std::this_thread::get_id() != trace.caller;
    if (trace.made[chunk] != 1) {
      trace.made_unprepared_consumed = true;
    }
    trace.consume_order.push_back(chunk);
    trace.held--;
  };
  return steps;
}

std::vector<size_t> Sequence(size_t count) {
  std::vector<size_t> all(count);
  for (size_t i = 0; i < count; i++) {
    all[i] = i;
  }
  return all;
}

}  // namespace

TEST_CASE("RunOrderedChunks prepares and consumes every chunk in order",
          "[device-access][chunks]") {
  const size_t threads = GENERATE(size_t{1}, size_t{2}, size_t{3}, size_t{8});
  const size_t window = GENERATE(size_t{1}, size_t{3}, size_t{16});
  constexpr size_t chunk_count = 200;
  INFO("threads " << threads << " window " << window);
  Trace trace(chunk_count);

  RunOrderedChunks(
      {.chunk_count = chunk_count, .threads = threads, .window = window},
      TracingSteps(trace));

  CHECK(trace.prepare_order == Sequence(chunk_count));
  CHECK(trace.consume_order == Sequence(chunk_count));
  for (const auto& times : trace.made) {
    CHECK(times == 1);
  }
  CHECK_FALSE(trace.made_unprepared);
  CHECK_FALSE(trace.made_unprepared_consumed);
  CHECK_FALSE(trace.step_off_caller);
}

TEST_CASE("RunOrderedChunks holds no more chunks than its window",
          "[device-access][chunks]") {
  const size_t threads = GENERATE(size_t{1}, size_t{4});
  const size_t window = GENERATE(size_t{1}, size_t{2}, size_t{5});
  Trace trace(100);

  RunOrderedChunks({.chunk_count = 100, .threads = threads, .window = window},
                   TracingSteps(trace));

  INFO("threads " << threads << " window " << window);
  CHECK(trace.most_held <= window);
  CHECK(trace.most_held >= 1);
}

TEST_CASE("RunOrderedChunks counts a window of 0 as 1",
          "[device-access][chunks]") {
  Trace trace(20);

  RunOrderedChunks({.chunk_count = 20, .threads = 3, .window = 0},
                   TracingSteps(trace));

  CHECK(trace.most_held == 1);
  CHECK(trace.consume_order == Sequence(20));
}

TEST_CASE("RunOrderedChunks gives each running call a slot of its own",
          "[device-access][chunks]") {
  constexpr size_t threads = 4;
  Trace trace(300);

  RunOrderedChunks({.chunk_count = 300, .threads = threads, .window = 12},
                   TracingSteps(trace));

  CHECK_FALSE(trace.shared_slot);
  CHECK(trace.highest_slot < threads);
}

TEST_CASE(
    "RunOrderedChunks makes the chunks on the calling thread with one "
    "thread",
    "[device-access][chunks]") {
  const std::thread::id caller = std::this_thread::get_id();
  std::vector<std::thread::id> makers;
  ChunkSteps steps;
  steps.prepare = [](size_t) {};
  steps.make = [&makers](size_t, size_t slot) {
    makers.push_back(std::this_thread::get_id());
    CHECK(slot == 0);
  };
  steps.consume = [](size_t) {};

  RunOrderedChunks({.chunk_count = 10, .threads = 1, .window = 4}, steps);

  REQUIRE(makers.size() == 10);
  CHECK(std::ranges::all_of(makers,
                            [caller](const auto& id) { return id == caller; }));
}

TEST_CASE("RunOrderedChunks takes no more threads than it has chunks",
          "[device-access][chunks]") {
  std::atomic<size_t> highest_slot{0};
  ChunkSteps steps;
  steps.prepare = [](size_t) {};
  steps.make = [&highest_slot](size_t, size_t slot) {
    highest_slot = (std::max)(highest_slot.load(), slot);
  };
  steps.consume = [](size_t) {};

  RunOrderedChunks({.chunk_count = 2, .threads = 64, .window = 8}, steps);

  CHECK(highest_slot < 2);
}

TEST_CASE("RunOrderedChunks does nothing for no chunk",
          "[device-access][chunks]") {
  bool called = false;
  ChunkSteps steps;
  steps.prepare = [&called](size_t) { called = true; };
  steps.make = [&called](size_t, size_t) { called = true; };
  steps.consume = [&called](size_t) { called = true; };

  RunOrderedChunks({.chunk_count = 0, .threads = 4, .window = 4}, steps);

  CHECK_FALSE(called);
}

TEST_CASE("RunOrderedChunks hands a chunk the data its prepare wrote",
          "[device-access][chunks]") {
  // prepare() fills a slot of a ring, make() sums it, consume() reads the sum:
  // the order of the steps is the only thing that keeps the ring right.
  constexpr size_t chunk_count = 500;
  constexpr size_t window = 6;
  const size_t threads = GENERATE(size_t{1}, size_t{4});
  std::vector<size_t> input(window, 0);
  std::vector<size_t> output(window, 0);
  std::vector<size_t> seen;
  ChunkSteps steps;
  steps.prepare = [&](size_t chunk) { input[chunk % window] = chunk * 3 + 1; };
  steps.make = [&](size_t chunk, size_t) {
    output[chunk % window] = input[chunk % window] * 2;
  };
  steps.consume = [&](size_t chunk) { seen.push_back(output[chunk % window]); };

  RunOrderedChunks(
      {.chunk_count = chunk_count, .threads = threads, .window = window},
      steps);

  REQUIRE(seen.size() == chunk_count);
  for (size_t chunk = 0; chunk < chunk_count; chunk++) {
    CHECK(seen[chunk] == (chunk * 3 + 1) * 2);
  }
}

TEST_CASE("RunOrderedChunks rethrows what a step threw",
          "[device-access][chunks]") {
  const size_t threads = GENERATE(size_t{1}, size_t{4});
  enum class Failing { Prepare, Make, Consume };
  const Failing failing =
      GENERATE(Failing::Prepare, Failing::Make, Failing::Consume);
  ChunkSteps steps;
  steps.prepare = [failing](size_t chunk) {
    if (failing == Failing::Prepare && chunk == 7) {
      throw std::runtime_error("prepare");
    }
  };
  steps.make = [failing](size_t chunk, size_t) {
    if (failing == Failing::Make && chunk == 7) {
      throw std::runtime_error("make");
    }
  };
  steps.consume = [failing](size_t chunk) {
    if (failing == Failing::Consume && chunk == 7) {
      throw std::runtime_error("consume");
    }
  };

  CHECK_THROWS_AS(
      RunOrderedChunks({.chunk_count = 50, .threads = threads, .window = 5},
                       steps),
      std::runtime_error);
}

TEST_CASE("RunOrderedChunks can run again after a failure",
          "[device-access][chunks]") {
  ChunkSteps failing;
  failing.prepare = [](size_t) {};
  failing.make = [](size_t, size_t) { throw std::runtime_error("make"); };
  failing.consume = [](size_t) {};
  CHECK_THROWS_AS(
      RunOrderedChunks({.chunk_count = 10, .threads = 3, .window = 4}, failing),
      std::runtime_error);

  Trace trace(10);
  RunOrderedChunks({.chunk_count = 10, .threads = 3, .window = 4},
                   TracingSteps(trace));

  CHECK(trace.consume_order == Sequence(10));
}
