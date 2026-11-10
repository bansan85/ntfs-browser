#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "cache/block-cache.h"
#include "cache/stats.h"
#include "io/run-block-loader.h"
#include "win-types.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::Io::RunBlockLoader;

namespace {

// Bytes of one block of the test cache.
constexpr std::int64_t block_bytes = 512;

// Blocks of the backing store: more than any run of the tests.
constexpr std::uint64_t store_blocks = 8;

// A cache, a backing store of numbered blocks, and the reads made on it.
struct LoaderRig {
  explicit LoaderRig(std::int64_t max_run_bytes, std::uint64_t last_index)
      : cache(static_cast<std::size_t>(block_bytes), Cache::unlimited_bytes),
        store(static_cast<std::size_t>(block_bytes) * store_blocks),
        loader(cache,
               {.block_bytes = block_bytes,
                .max_run_bytes = max_run_bytes,
                .last_index = last_index},
               [this](LARGE_INTEGER& addr, std::span<BYTE> dest) {
                 reads.push_back(addr.QuadPart);
                 if (fail_reads) {
                   return false;
                 }
                 std::ranges::copy(
                     std::span<const BYTE>(store).subspan(
                         static_cast<std::size_t>(addr.QuadPart), dest.size()),
                     dest.begin());
                 return true;
               }) {
    for (std::size_t i = 0; i < store.size(); i++) {
      store.at(i) =
          static_cast<BYTE>(i / static_cast<std::size_t>(block_bytes));
    }
  }

  // The block at index, as the loader gives it.
  std::shared_ptr<Cache::Block> Get(std::uint64_t index) {
    return loader.Get({.QuadPart = static_cast<LONGLONG>(index) * block_bytes});
  }

  Cache::BlockCache cache;
  std::vector<BYTE> store;
  bool fail_reads{false};
  // The address of every disk read, in order.
  std::vector<LONGLONG> reads;
  RunBlockLoader loader;
};

}  // namespace

TEST_CASE("A block that is missing brings in the run up to the last block",
          "[file-reader][cache][run][run-block-loader]") {
  LoaderRig rig(block_bytes * 4, 3);
  const std::shared_ptr<Cache::Block> first = rig.Get(0);
  REQUIRE(first);
  CHECK(first->bytes[0] == 0);
  CHECK(rig.reads.size() == 1);

  const std::shared_ptr<Cache::Block> last = rig.Get(3);
  REQUIRE(last);
  CHECK(last->bytes[0] == 3);
  CHECK(rig.reads.size() == 1);
}

TEST_CASE("A run never reaches past the last block of the range",
          "[file-reader][cache][run][run-block-loader]") {
  LoaderRig rig(block_bytes * 4, 1);
  REQUIRE(rig.Get(0));
  CHECK_FALSE(rig.cache.Contains(2));
  REQUIRE(rig.Get(1));
  CHECK(rig.reads.size() == 1);
}

TEST_CASE("A cached block is returned without a read",
          "[file-reader][cache][run][run-block-loader]") {
  LoaderRig rig(block_bytes, 0);
  const std::shared_ptr<Cache::Block> first = rig.Get(2);
  REQUIRE(first);
  CHECK(rig.Get(2) == first);
  CHECK(rig.reads.size() == 1);
}

TEST_CASE("A failed read gives no block and leaves nothing cached",
          "[file-reader][cache][run][run-block-loader]") {
  LoaderRig rig(block_bytes * 4, 3);
  rig.fail_reads = true;
  CHECK_FALSE(rig.Get(0));
  CHECK_FALSE(rig.cache.Contains(0));
}

TEST_CASE("A run that fails is not tried again in the same read",
          "[file-reader][cache][run][run-block-loader]") {
  LoaderRig rig(block_bytes * 4, 3);
  rig.fail_reads = true;
  CHECK_FALSE(rig.Get(0));
  // One failed run, then the block on its own.
  CHECK(rig.reads.size() == 2);
  rig.fail_reads = false;
  REQUIRE(rig.Get(1));
  // The block alone: no second run.
  CHECK(rig.reads.size() == 3);
  CHECK_FALSE(rig.cache.Contains(2));
}
