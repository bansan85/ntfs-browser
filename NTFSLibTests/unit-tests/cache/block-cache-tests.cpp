#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/namespace.h>

#include "block-cache-test-support.h"
#include "cache/block-cache.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

TEST_CASE("BlockCache evicts the least recently used block first",
          "[cache][block-cache]") {
  // Room for two of the four blocks.
  Cache::BlockCache cache(cache_test_block, 2 * cache_test_block);
  for (std::uint64_t index = 0; index < 3; index++) {
    REQUIRE(LoadTestBlock(cache, index));
  }
  // A hit counts as a use: block 0 is now newer than 1 and 2.
  REQUIRE(cache.Find(0));
  REQUIRE(LoadTestBlock(cache, 3));

  cache.Trim();

  CHECK(cache.HeldBytes() == 2 * cache_test_block);
  CHECK_FALSE(cache.Find(1));
  CHECK_FALSE(cache.Find(2));
  CHECK(cache.Find(0));
  CHECK(cache.Find(3));
}

TEST_CASE("BlockCache releases the fewest blocks needed",
          "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, 3 * cache_test_block);
  for (std::uint64_t index = 0; index < 4; index++) {
    REQUIRE(LoadTestBlock(cache, index));
  }

  cache.Trim();

  CHECK(cache.HeldBytes() == 3 * cache_test_block);
  CHECK_FALSE(cache.Find(0));
  CHECK(cache.Find(1));
  CHECK(cache.Find(2));
  CHECK(cache.Find(3));
}

TEST_CASE("BlockCache leaves a block in use alone and stays above the ceiling",
          "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, cache_test_block);
  const std::shared_ptr<Cache::Block> oldest = LoadTestBlock(cache, 0);
  const std::shared_ptr<Cache::Block> newest = LoadTestBlock(cache, 1);
  REQUIRE(LoadTestBlock(cache, 2));

  cache.Trim();

  // Both held blocks stay, so the total cannot reach the ceiling.
  CHECK(cache.HeldBytes() == 2 * cache_test_block);
  CHECK(cache.InUseBytes() == 2 * cache_test_block);
  CHECK(cache.Find(0) == oldest);
  CHECK(cache.Find(1) == newest);
  CHECK_FALSE(cache.Find(2));
}

TEST_CASE("BlockCache skips a block in use and frees the next one",
          "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, cache_test_block);
  const std::shared_ptr<Cache::Block> pinned = LoadTestBlock(cache, 0);
  REQUIRE(LoadTestBlock(cache, 1));
  REQUIRE(LoadTestBlock(cache, 2));

  cache.Trim();

  CHECK(cache.HeldBytes() == cache_test_block);
  CHECK(cache.Find(0) == pinned);
}

TEST_CASE("BlockCache frees a block once its last holder lets go",
          "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, 0);
  std::shared_ptr<Cache::Block> held = LoadTestBlock(cache, 0);
  cache.Trim();
  REQUIRE(cache.HeldBytes() == cache_test_block);

  held.reset();
  cache.Trim();

  CHECK(cache.HeldBytes() == 0);
}

TEST_CASE(
    "BlockCache never inserts a block whose read failed or came back "
    "short",
    "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, Cache::unlimited_bytes);

  const auto failed =
      cache.GetOrLoad(7, [](std::span<BYTE> /*dest*/) { return false; });

  CHECK_FALSE(failed);
  CHECK(cache.HeldBytes() == 0);
  CHECK_FALSE(cache.Find(7));
}

TEST_CASE("BlockCache loads a missing block once and serves it from then on",
          "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, Cache::unlimited_bytes);
  int loads = 0;
  const auto fill = [&loads](std::span<BYTE> dest) {
    loads++;
    std::ranges::fill(dest, BYTE{0x5A});
    return true;
  };

  const std::shared_ptr<Cache::Block> first = cache.GetOrLoad(3, fill);
  const std::shared_ptr<Cache::Block> second = cache.GetOrLoad(3, fill);

  REQUIRE(first);
  CHECK(second == first);
  CHECK(loads == 1);
  CHECK(first->Bytes().size() == cache_test_block);
  CHECK(first->Bytes().front() == BYTE{0x5A});
}

TEST_CASE(
    "BlockCache lets an exception from a read through and caches "
    "nothing",
    "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, Cache::unlimited_bytes);

  CHECK_THROWS_AS(cache.GetOrLoad(1,
                                  [](std::span<BYTE> /*dest*/) -> bool {
                                    throw std::runtime_error("read failed");
                                  }),
                  std::runtime_error);

  CHECK(cache.HeldBytes() == 0);
}

TEST_CASE("BlockCache keeps the block it already holds on a second insert",
          "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, Cache::unlimited_bytes);
  const std::shared_ptr<Cache::Block> first = LoadTestBlock(cache, 4);

  cache.Insert(4, std::make_shared<Cache::Block>(cache_test_block));

  CHECK(cache.Find(4) == first);
  CHECK(cache.HeldBytes() == cache_test_block);
}

TEST_CASE("BlockCache saturates its totals instead of wrapping",
          "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, Cache::unlimited_bytes);
  *cache.StitchedTally() = Cache::unlimited_bytes - 1;
  const std::shared_ptr<Cache::Block> block = LoadTestBlock(cache, 0);

  CHECK(cache.HeldBytes() == Cache::unlimited_bytes);
  CHECK(cache.InUseBytes() == Cache::unlimited_bytes);
}

TEST_CASE("BlockCache counts a live stitched buffer in both totals",
          "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, Cache::unlimited_bytes);
  REQUIRE(LoadTestBlock(cache, 0));
  constexpr std::size_t stitched_size = 100;

  {
    const Cache::BlockCache::Stitched stitched =
        cache.NewStitched(stitched_size);
    CHECK(stitched.bytes.size() == stitched_size);
    CHECK(cache.HeldBytes() == cache_test_block + stitched_size);
    CHECK(cache.InUseBytes() == stitched_size);
    CHECK(cache.GetStats().held_bytes == cache_test_block + stitched_size);
    CHECK(cache.GetStats().in_use_bytes == stitched_size);
  }

  CHECK(cache.HeldBytes() == cache_test_block);
  CHECK(cache.InUseBytes() == 0);
}

TEST_CASE(
    "BlockCache frees an unused block even when a stitched buffer is "
    "alive",
    "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, cache_test_block);
  REQUIRE(LoadTestBlock(cache, 0));
  const Cache::BlockCache::Stitched stitched = cache.NewStitched(8);

  cache.Trim();

  // The tally alone is under the ceiling, so only the block had to go.
  CHECK(cache.HeldBytes() == 8);
}

TEST_CASE("BlockCache reports the block size it was built with",
          "[cache][block-cache]") {
  const Cache::BlockCache cache(cache_test_block, 0);

  CHECK(cache.BlockBytes() == cache_test_block);
}

TEST_CASE(
    "BlockCache Minify frees exactly the unused blocks and keeps the "
    "order of the rest",
    "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, Cache::unlimited_bytes);
  const std::shared_ptr<Cache::Block> pinned_old = LoadTestBlock(cache, 0);
  REQUIRE(LoadTestBlock(cache, 1));
  const std::shared_ptr<Cache::Block> pinned_new = LoadTestBlock(cache, 2);
  REQUIRE(LoadTestBlock(cache, 3));

  cache.Minify();

  CHECK(cache.HeldBytes() == 2 * cache_test_block);
  CHECK(cache.InUseBytes() == 2 * cache_test_block);
  CHECK_FALSE(cache.Find(1));
  CHECK_FALSE(cache.Find(3));
  CHECK(cache.Find(0) == pinned_old);
  CHECK(cache.Find(2) == pinned_new);
}

TEST_CASE("BlockCache Minify leaves the least recently used order alone",
          "[cache][block-cache]") {
  Cache::BlockCache cache(cache_test_block, cache_test_block);
  std::shared_ptr<Cache::Block> first = LoadTestBlock(cache, 0);
  std::shared_ptr<Cache::Block> second = LoadTestBlock(cache, 1);
  REQUIRE(LoadTestBlock(cache, 2));
  cache.Minify();
  first.reset();
  second.reset();

  cache.Trim();

  // Block 0 was the older of the two that stayed, so it is the one that goes.
  CHECK(cache.HeldBytes() == cache_test_block);
  CHECK_FALSE(cache.Find(0));
  CHECK(cache.Find(1));
}
