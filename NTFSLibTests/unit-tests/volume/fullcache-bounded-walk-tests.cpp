#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/namespace.h>

#include "fake-ntfs-image.h"
#include "fullcache-walk-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// A ceiling the walk image never reaches: 16 MiB, against its 2 MiB of index
// data.
constexpr std::uint64_t walk_roomy_ceiling = 16ULL * 1024 * 1024;

// Views the in-use case holds: eight blocks, so 512 KiB of memory the cache
// cannot free, well over the tiny ceiling.
constexpr std::size_t held_view_count = 8;

// Bytes each held view covers.
constexpr DWORD held_view_length = 64;

}  // namespace

TEST_CASE(
    "FullCache walk under a tiny ceiling lists what every other walk "
    "lists",
    "[cache][walk]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const std::vector<std::wstring> expected = ExpectedWalkNames();

  const auto tiny = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(walk_tiny_ceiling));
  const auto roomy = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(walk_roomy_ceiling));
  const auto unlimited = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(Cache::unlimited_bytes));
  const auto no_cache = OpenWalkVolume<Cache::Strategy::NoCache>(
      image, WalkOptions(Cache::default_max_cache_bytes));

  CHECK(WalkRoot(*tiny) == expected);
  CHECK(WalkRoot(*roomy) == expected);
  CHECK(WalkRoot(*unlimited) == expected);
  CHECK(WalkRoot(*no_cache) == expected);

  CHECK(tiny->GetCacheStats().held_bytes <= walk_tiny_ceiling);
  // The roomy ceiling is never reached, so nothing was evicted.
  CHECK(roomy->GetCacheStats().held_bytes > walk_tiny_ceiling);
  CHECK(roomy->GetCacheStats().held_bytes <= walk_roomy_ceiling);
  CHECK(unlimited->GetCacheStats().held_bytes ==
        roomy->GetCacheStats().held_bytes);
  CHECK(no_cache->GetCacheStats().held_bytes == 0);
}

TEST_CASE(
    "FullCache walk keeps data in use, even above the ceiling, and falls "
    "back under it once that data is released",
    "[cache][walk]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto volume = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(walk_tiny_ceiling));

  // One view per block, kept open through the walk.
  std::vector<Cache::View> held;
  std::vector<std::vector<BYTE>> snapshots;
  for (std::size_t block = 0; block < held_view_count; block++) {
    LARGE_INTEGER addr{.QuadPart =
                           static_cast<LONGLONG>(block * walk_cache_block)};
    std::optional<Cache::View> view = volume->Read(addr, held_view_length);
    REQUIRE(view.has_value());
    snapshots.emplace_back(view->data(), view->data() + view->size());
    held.push_back(*view);
  }

  CHECK(WalkRoot(*volume) == ExpectedWalkNames());

  // The held blocks stay, so the total cannot be brought under the ceiling.
  const Cache::Stats busy = volume->GetCacheStats();
  CHECK(busy.in_use_bytes == held_view_count * walk_cache_block);
  CHECK(busy.held_bytes >= busy.in_use_bytes);
  CHECK(busy.held_bytes > walk_tiny_ceiling);
  for (std::size_t i = 0; i < held.size(); i++) {
    const std::span<const BYTE> bytes = held.at(i);
    CHECK(std::equal(bytes.begin(), bytes.end(), snapshots.at(i).begin()));
  }

  held.clear();

  CHECK(volume->GetCacheStats().held_bytes <= walk_tiny_ceiling);
  CHECK(volume->GetCacheStats().in_use_bytes == 0);
}
