#include <ntfs-browser/win-types.h>

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "fake-ntfs-image.h"
#include "fullcache-walk-support.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// Walks the root this many times, to show that nothing piles up from one walk
// to the next.
constexpr int repeated_walks = 20;

}  // namespace

TEST_CASE("FullCache repeated walk stays under the ceiling",
          "[cache][walk][regression]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto volume = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(walk_tiny_ceiling));
  const std::vector<std::wstring> expected = ExpectedWalkNames();

  for (int walk = 0; walk < repeated_walks; walk++) {
    INFO("walk " << walk);
    CHECK(WalkRoot(*volume) == expected);
    CHECK(volume->GetCacheStats().held_bytes <= walk_tiny_ceiling);
  }
}

TEST_CASE("FullCache repeated walk grows past the ceiling without one",
          "[cache][walk][regression]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto volume = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(Cache::unlimited_bytes));
  const std::vector<std::wstring> expected = ExpectedWalkNames();

  for (int walk = 0; walk < repeated_walks; walk++) {
    INFO("walk " << walk);
    CHECK(WalkRoot(*volume) == expected);
  }
  CHECK(volume->GetCacheStats().held_bytes > walk_tiny_ceiling);
}
