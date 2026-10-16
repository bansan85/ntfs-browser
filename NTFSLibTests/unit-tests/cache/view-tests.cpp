#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/namespace.h>

#include "io/file-reader.h"
#include "memory-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using FullReader =
    NTFS_BROWSER_NAMESPACE::Io::FileReader<Cache::Strategy::FullCache>;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;

namespace {

// Bytes of one block of FileReader<FullCache>.
constexpr std::uint64_t view_block = 65536;

// Blocks of the backing store: enough for a read to straddle two of them and
// to leave a third for the reads that must not disturb it.
constexpr std::size_t view_backing_blocks = 3;

// Length of the reads in these tests: small, so each stays inside one block
// unless a test moves its address to a boundary.
constexpr DWORD view_read_length = 100;

// Backing store whose bytes depend on their offset, so a view over the wrong
// range cannot match by accident.
[[nodiscard]] std::vector<BYTE> PatternBacking() {
  std::vector<BYTE> backing(view_backing_blocks * view_block);
  for (std::size_t i = 0; i < backing.size(); i++) {
    // i < backing.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    backing[i] = static_cast<BYTE>((i * 7) ^ (i >> 8));
  }
  return backing;
}

[[nodiscard]] std::unique_ptr<FullReader> MakeReader(std::uint64_t ceiling) {
  return std::make_unique<FullReader>(
      std::make_unique<MemoryDiskReader>(PatternBacking()), ceiling);
}

// A read of view_read_length bytes at the start of block "block".
[[nodiscard]] std::optional<Cache::View> ReadInBlock(const FullReader& reader,
                                                     std::uint64_t block) {
  LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(block * view_block)};
  return reader.Read(addr, view_read_length);
}

[[nodiscard]] bool MatchesBacking(std::span<const BYTE> bytes,
                                  std::uint64_t offset) {
  const std::vector<BYTE> backing = PatternBacking();
  return std::equal(bytes.begin(), bytes.end(),
                    backing.begin() + static_cast<std::ptrdiff_t>(offset));
}

}  // namespace

TEST_CASE("View copies share the memory and keep the data valid",
          "[cache][view]") {
  const auto reader = MakeReader(0);
  std::optional<Cache::View> original = ReadInBlock(*reader, 1);
  REQUIRE(original.has_value());

  std::optional<Cache::View> copy = original;
  original.reset();

  REQUIRE(copy.has_value());
  CHECK(MatchesBacking(*copy, view_block));
  CHECK(reader->GetCacheStats().in_use_bytes == view_block);

  copy.reset();

  CHECK(reader->GetCacheStats().held_bytes == 0);
}

TEST_CASE("View dropping the last copy of a block trims an over-full cache",
          "[cache][view]") {
  const auto reader = MakeReader(view_block);
  std::optional<Cache::View> first = ReadInBlock(*reader, 0);
  std::optional<Cache::View> second = ReadInBlock(*reader, 1);
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(reader->GetCacheStats().held_bytes == 2 * view_block);

  first.reset();

  CHECK(reader->GetCacheStats().held_bytes == view_block);
  CHECK(reader->GetCacheStats().in_use_bytes == view_block);
}

TEST_CASE("View keeps the block of another copy when one copy is dropped",
          "[cache][view]") {
  const auto reader = MakeReader(0);
  std::optional<Cache::View> first = ReadInBlock(*reader, 0);
  REQUIRE(first.has_value());
  const std::optional<Cache::View> second = first;

  first.reset();

  CHECK(reader->GetCacheStats().held_bytes == view_block);
}

TEST_CASE("View moved-from does nothing when it is dropped", "[cache][view]") {
  const auto reader = MakeReader(0);
  std::optional<Cache::View> source = ReadInBlock(*reader, 0);
  REQUIRE(source.has_value());

  const Cache::View target = std::move(*source);
  source.reset();

  CHECK(reader->GetCacheStats().held_bytes == view_block);
  CHECK(MatchesBacking(target, 0));
  CHECK(source.has_value() == false);
}

TEST_CASE("View moved-from is empty", "[cache][view]") {
  const auto reader = MakeReader(Cache::unlimited_bytes);
  std::optional<Cache::View> source = ReadInBlock(*reader, 0);
  REQUIRE(source.has_value());

  const Cache::View target = std::move(*source);

  // NOLINTNEXTLINE(bugprone-use-after-move): checking the moved-from state.
  CHECK(source->empty());
  CHECK(target.size() == view_read_length);
}

TEST_CASE("View assignment releases what the view held before",
          "[cache][view]") {
  const auto reader = MakeReader(0);
  std::optional<Cache::View> held = ReadInBlock(*reader, 0);
  std::optional<Cache::View> other = ReadInBlock(*reader, 1);
  REQUIRE(held.has_value());
  REQUIRE(other.has_value());
  CHECK(reader->GetCacheStats().held_bytes == 2 * view_block);

  SECTION("copy assignment") {
    *held = *other;

    CHECK(reader->GetCacheStats().held_bytes == view_block);
    CHECK(MatchesBacking(*held, view_block));
  }

  SECTION("move assignment") {
    *held = std::move(*other);

    CHECK(reader->GetCacheStats().held_bytes == view_block);
    CHECK(MatchesBacking(*held, view_block));
  }

  SECTION("self assignment") {
    const Cache::View& alias = *held;
    *held = alias;

    CHECK(reader->GetCacheStats().held_bytes == 2 * view_block);
    CHECK(MatchesBacking(*held, 0));
  }
}

TEST_CASE("View default-constructed holds nothing", "[cache][view]") {
  const Cache::View view;

  CHECK(view.empty());
  CHECK(view.size() == 0);
  CHECK(view.data() == nullptr);
  const std::span<const BYTE> bytes = view;
  CHECK(bytes.empty());
}

TEST_CASE("View dropped after its volume is gone does not crash",
          "[cache][view]") {
  auto reader = MakeReader(0);
  std::optional<Cache::View> view = ReadInBlock(*reader, 0);
  REQUIRE(view.has_value());

  reader.reset();

  CHECK_NOTHROW(view.reset());
}

TEST_CASE("View over a stitched read counts in the tally until it is dropped",
          "[cache][view]") {
  const auto reader = MakeReader(Cache::unlimited_bytes);
  LARGE_INTEGER addr{.QuadPart = static_cast<LONGLONG>(view_block - 50)};
  std::optional<Cache::View> view = reader->Read(addr, view_read_length);
  REQUIRE(view.has_value());
  CHECK(MatchesBacking(*view, view_block - 50));

  // Two cached blocks, plus the stitched copy the view owns.
  CHECK(reader->GetCacheStats().held_bytes ==
        (2 * view_block) + view_read_length);
  CHECK(reader->GetCacheStats().in_use_bytes == view_read_length);

  view.reset();

  CHECK(reader->GetCacheStats().held_bytes == 2 * view_block);
  CHECK(reader->GetCacheStats().in_use_bytes == 0);
}
