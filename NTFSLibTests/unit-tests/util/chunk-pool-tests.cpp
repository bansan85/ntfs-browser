#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "util/chunk-pool.h"

namespace Util = NTFS_BROWSER_NAMESPACE::Util;

TEST_CASE("every chunk of a batch runs once", "[util][chunk-pool]") {
  const size_t helpers = GENERATE(size_t{0}, size_t{1}, size_t{3});
  constexpr size_t chunks = 200;
  Util::ChunkPool pool(helpers);
  std::vector<std::atomic<int>> runs(chunks);

  pool.Start(chunks,
             [&runs](size_t chunk, size_t /*slot*/) { runs.at(chunk)++; });
  pool.WaitFor(chunks - 1);
  for (size_t chunk = 0; chunk < chunks; chunk++) {
    pool.WaitFor(chunk);
  }
  pool.Finish();

  INFO("helpers " << helpers);
  for (const std::atomic<int>& count : runs) {
    CHECK(count == 1);
  }
}

TEST_CASE("a chunk is done when WaitFor returns", "[util][chunk-pool]") {
  Util::ChunkPool pool(3);
  constexpr size_t chunks = 64;
  std::vector<int> results(chunks, 0);

  pool.Start(chunks, [&results](size_t chunk, size_t /*slot*/) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    results.at(chunk) = static_cast<int>(chunk) + 1;
  });
  for (size_t chunk = 0; chunk < chunks; chunk++) {
    pool.WaitFor(chunk);
    CHECK(results.at(chunk) == static_cast<int>(chunk) + 1);
  }
  pool.Finish();
}

TEST_CASE("no two chunks that run at once share a slot", "[util][chunk-pool]") {
  Util::ChunkPool pool(3);
  constexpr size_t chunks = 300;
  std::vector<std::atomic<bool>> busy(pool.Slots());
  std::atomic<int> clashes{0};
  std::atomic<int> out_of_range{0};

  pool.Start(chunks, [&](size_t /*chunk*/, size_t slot) {
    if (slot >= busy.size()) {
      out_of_range++;
      return;
    }
    if (busy.at(slot).exchange(true)) {
      clashes++;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(20));
    busy.at(slot) = false;
  });
  for (size_t chunk = 0; chunk < chunks; chunk++) {
    pool.WaitFor(chunk);
  }
  pool.Finish();

  CHECK(clashes == 0);
  CHECK(out_of_range == 0);
}

TEST_CASE("a pool without helpers runs the chunks in order on the caller",
          "[util][chunk-pool]") {
  Util::ChunkPool pool(0);
  CHECK(pool.Slots() == 1);
  std::vector<size_t> order;
  const std::thread::id caller = std::this_thread::get_id();
  bool elsewhere = false;

  pool.Start(20, [&](size_t chunk, size_t slot) {
    order.push_back(chunk);
    elsewhere = elsewhere || std::this_thread::get_id() != caller || slot != 0;
  });
  pool.WaitFor(19);
  pool.Finish();

  CHECK_FALSE(elsewhere);
  REQUIRE(order.size() == 20);
  for (size_t chunk = 0; chunk < order.size(); chunk++) {
    CHECK(order.at(chunk) == chunk);
  }
}

TEST_CASE("a pool with helpers has a slot for each thread",
          "[util][chunk-pool]") {
  Util::ChunkPool pool(3);

  CHECK(pool.Slots() == 4);
}

TEST_CASE("Finish drops the chunks no thread started", "[util][chunk-pool]") {
  Util::ChunkPool pool(0);
  std::atomic<int> runs{0};

  pool.Start(100, [&runs](size_t /*chunk*/, size_t /*slot*/) { runs++; });
  pool.WaitFor(2);
  pool.Finish();

  // Chunks 0 to 2 were needed. Nothing after them runs.
  CHECK(runs == 3);
}

TEST_CASE("a pool runs one batch after the other", "[util][chunk-pool]") {
  Util::ChunkPool pool(2);
  for (int batch = 0; batch < 5; batch++) {
    std::vector<std::atomic<int>> runs(40);
    pool.Start(40,
               [&runs](size_t chunk, size_t /*slot*/) { runs.at(chunk)++; });
    for (size_t chunk = 0; chunk < 40; chunk++) {
      pool.WaitFor(chunk);
    }
    pool.Finish();
    for (const std::atomic<int>& count : runs) {
      CHECK(count == 1);
    }
  }
}

TEST_CASE("an exception of a chunk reaches the thread that waits",
          "[util][chunk-pool]") {
  const size_t helpers = GENERATE(size_t{0}, size_t{2});
  Util::ChunkPool pool(helpers);

  pool.Start(10, [](size_t chunk, size_t /*slot*/) {
    if (chunk == 4) {
      throw std::runtime_error("chunk 4");
    }
  });

  INFO("helpers " << helpers);
  bool thrown = false;
  try {
    for (size_t chunk = 0; chunk < 10; chunk++) {
      pool.WaitFor(chunk);
    }
  } catch (const std::runtime_error& error) {
    thrown = true;
    CHECK(std::string(error.what()) == "chunk 4");
  }
  CHECK(thrown);
  CHECK_NOTHROW(pool.Finish());

  // The pool takes another batch after the failure.
  std::atomic<int> runs{0};
  pool.Start(5, [&runs](size_t /*chunk*/, size_t /*slot*/) { runs++; });
  for (size_t chunk = 0; chunk < 5; chunk++) {
    pool.WaitFor(chunk);
  }
  pool.Finish();
  CHECK(runs == 5);
}

TEST_CASE(
    "an exception of a chunk the waiting thread ran for another is not lost",
    "[util][chunk-pool]") {
  Util::ChunkPool pool(0);

  pool.Start(3, [](size_t chunk, size_t /*slot*/) {
    if (chunk == 1) {
      throw std::runtime_error("late");
    }
  });
  pool.WaitFor(0);
  // The caller runs chunk 1 while it waits for chunk 2, and chunk 1 fails.
  CHECK_THROWS_AS(pool.WaitFor(2), std::runtime_error);
  CHECK_NOTHROW(pool.Finish());
}

TEST_CASE("a pool that goes away mid-batch waits for the chunks it started",
          "[util][chunk-pool]") {
  std::atomic<int> finished{0};
  std::atomic<int> started{0};
  {
    Util::ChunkPool pool(3);
    pool.Start(1000, [&](size_t /*chunk*/, size_t /*slot*/) {
      started++;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      finished++;
    });
    pool.WaitFor(0);
  }

  // No chunk was left half done, and the batch was not run to its end.
  CHECK(started == finished);
  CHECK(started < 1000);
}
