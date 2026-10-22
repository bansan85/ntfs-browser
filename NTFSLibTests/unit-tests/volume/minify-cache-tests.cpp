#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/namespace.h>

#include "fullcache-walk-support.h"
#include "ntfs-browser/ntfs-volume.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;

namespace {

// Bytes the held view covers: a few, at the start of the image.
constexpr DWORD minify_view_length = 64;

}  // namespace

TEST_CASE("MinifyCache frees the whole cache once no view is left",
          "[cache][minify]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto volume = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(Cache::unlimited_bytes));
  const std::vector<std::wstring> expected = ExpectedWalkNames();
  REQUIRE(WalkRoot(*volume) == expected);
  REQUIRE(volume->GetCacheStats().held_bytes > 0);

  volume->MinifyCache();

  CHECK(volume->GetCacheStats().held_bytes == 0);
  CHECK(volume->IsVolumeOK());
  // The data loads again, and is the same.
  CHECK(WalkRoot(*volume) == expected);
  CHECK(volume->GetCacheStats().held_bytes > 0);
}

TEST_CASE("MinifyCache keeps the blocks a view still uses", "[cache][minify]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto volume = OpenWalkVolume<Cache::Strategy::FullCache>(
      image, WalkOptions(Cache::unlimited_bytes));
  REQUIRE(WalkRoot(*volume) == ExpectedWalkNames());

  LARGE_INTEGER addr{.QuadPart = 0};
  const std::optional<Cache::View> view =
      volume->Read(addr, minify_view_length);
  REQUIRE(view.has_value());
  const std::vector<BYTE> before(view->data(), view->data() + view->size());

  volume->MinifyCache();

  const Cache::Stats stats = volume->GetCacheStats();
  CHECK(stats.held_bytes == walk_cache_block);
  CHECK(stats.in_use_bytes == walk_cache_block);
  const std::span<const BYTE> bytes = *view;
  CHECK(std::equal(bytes.begin(), bytes.end(), before.begin()));
  CHECK(volume->IsVolumeOK());

  LARGE_INTEGER again{.QuadPart = 0};
  const std::optional<Cache::View> second =
      volume->Read(again, minify_view_length);
  REQUIRE(second.has_value());
  const std::span<const BYTE> second_bytes = *second;
  CHECK(std::equal(second_bytes.begin(), second_bytes.end(), before.begin()));
}

TEST_CASE("MinifyCache does nothing under NoCache", "[cache][minify]") {
  const std::vector<BYTE> image =
      NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithManyIndexBlocks(
          walk_index_blocks);
  const auto volume = OpenWalkVolume<Cache::Strategy::NoCache>(
      image, WalkOptions(Cache::unlimited_bytes));
  const std::vector<std::wstring> expected = ExpectedWalkNames();

  volume->MinifyCache();

  CHECK(volume->GetCacheStats().held_bytes == 0);
  CHECK(WalkRoot(*volume) == expected);
}
