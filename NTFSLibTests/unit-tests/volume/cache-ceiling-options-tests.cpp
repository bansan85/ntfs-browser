#include <ntfs-browser/win-types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/volume-options.h>

#include "fullcache-walk-support.h"
#include "ntfs-browser/ntfs-volume.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// 256 MiB, spelled out: the documented default, not the constant under test.
constexpr std::uint64_t documented_default = 256ULL * 1024 * 1024;

// The two ceilings of the per-volume test: one block, and four.
constexpr std::uint64_t small_ceiling = walk_cache_block;
constexpr std::uint64_t larger_ceiling = 4 * walk_cache_block;

}  // namespace

TEST_CASE("VolumeOptions defaults the cache ceiling to 256 MiB",
          "[cache][options]") {
  const NTFS_BROWSER_NAMESPACE::VolumeOptions options;

  CHECK(options.max_cache_bytes == documented_default);
  CHECK(options.max_cache_bytes == Cache::default_max_cache_bytes);
}

TEST_CASE("A volume reads its ceiling back through GetOptions",
          "[cache][options]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto by_default = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, NTFS_BROWSER_NAMESPACE::VolumeOptions{});
  const auto chosen = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(larger_ceiling));

  CHECK(by_default->GetOptions().max_cache_bytes == documented_default);
  CHECK(chosen->GetOptions().max_cache_bytes == larger_ceiling);
}

TEST_CASE("Two volumes each stay within their own ceiling",
          "[cache][options]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto one_block_volume = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(small_ceiling));
  const auto four_block_volume = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(larger_ceiling));
  const std::vector<std::wstring> expected = ExpectedWalkNames();

  // Interleaved, so one volume's cache could leak into the other's.
  CHECK(WalkRoot(*one_block_volume) == expected);
  CHECK(WalkRoot(*four_block_volume) == expected);
  CHECK(WalkRoot(*one_block_volume) == expected);

  CHECK(one_block_volume->GetCacheStats().held_bytes <= small_ceiling);
  CHECK(four_block_volume->GetCacheStats().held_bytes <= larger_ceiling);
  // Both read the same index data, so the roomier cache holds more of it.
  CHECK(four_block_volume->GetCacheStats().held_bytes >
        one_block_volume->GetCacheStats().held_bytes);
}

TEST_CASE("NoCache ignores the ceiling and reports an empty cache",
          "[cache][options]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto volume = OpenWalkVolume<Cache::Strategy::NoCache>(
      image, WalkOptions(small_ceiling));

  CHECK(WalkRoot(*volume) == ExpectedWalkNames());

  CHECK(volume->GetCacheStats().held_bytes == 0);
  CHECK(volume->GetCacheStats().in_use_bytes == 0);
}

TEST_CASE("An unlimited ceiling keeps everything the walks read",
          "[cache][options]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto volume = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(Cache::unlimited_bytes));

  CHECK(WalkRoot(*volume) == ExpectedWalkNames());
  const std::uint64_t after_first = volume->GetCacheStats().held_bytes;
  CHECK(WalkRoot(*volume) == ExpectedWalkNames());

  // Nothing was evicted, and a second walk adds nothing.
  CHECK(after_first > walk_tiny_ceiling);
  CHECK(volume->GetCacheStats().held_bytes == after_first);
}
