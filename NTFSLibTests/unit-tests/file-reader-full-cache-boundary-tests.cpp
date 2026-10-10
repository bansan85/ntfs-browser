#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>

#include "io/file-reader.h"
#include "memory-disk-reader.h"
#include "optional-access.h"

namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::Io::FileReader;

namespace {

// Size of one FullCache block.
constexpr size_t cache_block_size = 65536;

// How far before a block boundary the straddling reads start, and how long
// they are.
constexpr size_t straddle_before = 36;
constexpr DWORD straddle_length = 100;

// Pattern multipliers: the byte at "i" is i * step, so a misplaced read
// cannot match by accident.
// Bits in a byte: the pattern mixes in the second byte of the index.
constexpr unsigned bits_per_byte = 8;
constexpr size_t pattern_step_a = 7;
constexpr size_t pattern_step_b = 3;

// Size of the partial block after the first full one, and the small backing
// store of the single-block test.
constexpr size_t tail_size = 4096;

// Offset, in the last block, of the repeated read, and the length of the
// reads inside it.
constexpr size_t inner_offset = 1024;
constexpr DWORD inner_length = 512;

// How far before the end the past-the-end read starts.
constexpr size_t past_end_before = 256;

}  // namespace

TEST_CASE(
    "FileReader<FullCache>::Read must not abort/misbehave on a read "
    "crossing a 64KiB cache block boundary",
    "[file-reader][regression]") {
  // Each byte distinct from its offset, so returned data can be checked
  // exactly.
  std::vector<BYTE> backing(2 * cache_block_size);
  for (size_t i = 0; i < backing.size(); i++) {
    // i < backing.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    backing[i] = static_cast<BYTE>(i);
  }

  auto reader_double =
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing);
  const FileReader<Cache::Strategy::FullCache> reader(std::move(reader_double));

  // Straddles the boundary between the first and second 64KiB cache blocks.
  LARGE_INTEGER addr{.QuadPart = cache_block_size - straddle_before};
  constexpr DWORD length_value = straddle_length;

  // A crossing read may abort the process; ctest reports that as this
  // test failing.
  const std::optional<std::span<const BYTE>> result =
      reader.Read(addr, length_value);

  if (result) {
    CHECK(result->size() == length_value);
    for (size_t i = 0; i < result->size(); i++) {
      // i < result->size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      CHECK((*result)[i] == backing.at(cache_block_size - straddle_before + i));
    }
  }
}

TEST_CASE(
    "FileReader<FullCache>::Read reads a range inside the last partial "
    "64KiB block of the backing store like NoCache does",
    "[file-reader][regression]") {
  constexpr size_t block_value = cache_block_size;
  constexpr size_t tail = tail_size;

  std::vector<BYTE> backing(block_value + tail);
  for (size_t i = 0; i < backing.size(); i++) {
    // i < backing.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    backing[i] = static_cast<BYTE>(i * pattern_step_a + (i >> bits_per_byte));
  }

  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));
  const FileReader<Cache::Strategy::NoCache> exact(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));

  struct Range {
    size_t offset;
    DWORD length;
  };

  // Inside the partial block, ending exactly at the end, and straddling into
  // it from the full block before.
  const auto ranges =
      std::to_array<Range>({{block_value + inner_offset, inner_length},
                            {block_value + tail - inner_length, inner_length},
                            {block_value - straddle_before, straddle_length}});

  for (const Range& range : ranges) {
    LARGE_INTEGER addr_full{.QuadPart = gsl::narrow<LONGLONG>(range.offset)};
    LARGE_INTEGER addr_exact{.QuadPart = gsl::narrow<LONGLONG>(range.offset)};

    const auto expected = exact.Read(addr_exact, range.length);
    REQUIRE(expected.has_value());

    const auto actual = full.Read(addr_full, range.length);
    REQUIRE(actual.has_value());
    REQUIRE(NtfsBrowserTests::Unwrap(actual).size() ==
            NtfsBrowserTests::Unwrap(expected).size());
    CHECK(std::equal(NtfsBrowserTests::Unwrap(actual).begin(),
                     NtfsBrowserTests::Unwrap(actual).end(),
                     NtfsBrowserTests::Unwrap(expected).begin()));
    CHECK(std::equal(NtfsBrowserTests::Unwrap(actual).begin(),
                     NtfsBrowserTests::Unwrap(actual).end(),
                     backing.begin() + gsl::narrow<ptrdiff_t>(range.offset)));
  }

  // Reading the same range twice keeps returning the same bytes.
  LARGE_INTEGER again{.QuadPart =
                          static_cast<LONGLONG>(block_value + inner_offset)};
  const auto second = full.Read(again, inner_length);
  REQUIRE(second.has_value());
  CHECK(std::equal(NtfsBrowserTests::Unwrap(second).begin(),
                   NtfsBrowserTests::Unwrap(second).end(),
                   backing.begin() +
                       static_cast<ptrdiff_t>(block_value + inner_offset)));

  // A range that really runs past the end still fails, as in NoCache.
  LARGE_INTEGER past_end{
      .QuadPart = static_cast<LONGLONG>(block_value + tail - past_end_before)};
  CHECK_FALSE(full.Read(past_end, inner_length).has_value());
}

TEST_CASE(
    "FileReader<FullCache>::Read reads from a backing store smaller than "
    "one 64KiB block",
    "[file-reader][regression]") {
  std::vector<BYTE> backing(tail_size);
  for (size_t i = 0; i < backing.size(); i++) {
    // i < backing.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    backing[i] = static_cast<BYTE>(i * pattern_step_b);
  }

  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));

  LARGE_INTEGER addr{.QuadPart = inner_length};
  const auto result = full.Read(addr, inner_length);
  REQUIRE(result.has_value());
  REQUIRE(NtfsBrowserTests::Unwrap(result).size() == inner_length);
  CHECK(std::equal(NtfsBrowserTests::Unwrap(result).begin(),
                   NtfsBrowserTests::Unwrap(result).end(),
                   backing.begin() + inner_length));
}

TEST_CASE(
    "FileReader<FullCache>::Read rejects a negative address instead of "
    "returning a view before its cache block",
    "[file-reader][regression]") {
  const std::vector<BYTE> backing(cache_block_size);
  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));

  LARGE_INTEGER addr{.QuadPart = -1};
  CHECK_FALSE(full.Read(addr, 100).has_value());
}

TEST_CASE(
    "FileReader<FullCache>::Read rejects a range that runs past the largest "
    "signed address",
    "[file-reader][regression]") {
  const std::vector<BYTE> backing(cache_block_size);
  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));

  constexpr DWORD length_value = 100;
  const auto addresses = std::to_array<LONGLONG>(
      {std::numeric_limits<LONGLONG>::max(),
       std::numeric_limits<LONGLONG>::max() - 10,
       std::numeric_limits<LONGLONG>::max() - (length_value - 1)});
  for (const LONGLONG address : addresses) {
    INFO("address " << address);
    LARGE_INTEGER addr{.QuadPart = address};
    CHECK_FALSE(full.Read(addr, length_value).has_value());
  }
}
