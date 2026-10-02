#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/strategy.h>

#include "file-reader.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileReader;
using NtfsBrowser::Strategy;

namespace
{

// Size of one FULL_CACHE block.
constexpr size_t kCacheBlockSize = 65536;

// How far before a block boundary the straddling reads start, and how long
// they are.
constexpr size_t kStraddleBefore = 36;
constexpr DWORD kStraddleLength = 100;

// Pattern multipliers: the byte at "i" is i * step, so a misplaced read
// cannot match by accident.
// Bits in a byte: the pattern mixes in the second byte of the index.
constexpr unsigned kBitsPerByte = 8;
constexpr size_t kPatternStepA = 7;
constexpr size_t kPatternStepB = 3;

// Size of the partial block after the first full one, and the small backing
// store of the single-block test.
constexpr size_t kTailSize = 4096;

// Offset, in the last block, of the repeated read, and the length of the
// reads inside it.
constexpr size_t kInnerOffset = 1024;
constexpr DWORD kInnerLength = 512;

// How far before the end the past-the-end read starts.
constexpr size_t kPastEndBefore = 256;

}  // namespace

TEST_CASE(
    "FileReader<FULL_CACHE>::Read must not abort/misbehave on a read "
    "crossing a 64KiB cache block boundary",
    "[file-reader][regression]")
{
  // Each byte distinct from its offset, so returned data can be checked
  // exactly.
  std::vector<BYTE> backing(2 * kCacheBlockSize);
  for (size_t i = 0; i < backing.size(); i++)
  {
    backing[i] = static_cast<BYTE>(i);
  }

  auto reader_double =
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing);
  FileReader<Strategy::FULL_CACHE> reader(std::move(reader_double));

  // Straddles the boundary between the first and second 64KiB cache blocks.
  LARGE_INTEGER addr{.QuadPart = kCacheBlockSize - kStraddleBefore};
  constexpr DWORD kLength = kStraddleLength;

  // A crossing read may abort the process; ctest reports that as this
  // test failing.
  const std::optional<std::span<const BYTE>> result =
      reader.Read(addr, kLength);

  if (result)
  {
    CHECK(result->size() == kLength);
    for (size_t i = 0; i < result->size(); i++)
    {
      CHECK((*result)[i] == backing[kCacheBlockSize - kStraddleBefore + i]);
    }
  }
}

TEST_CASE(
    "FileReader<FULL_CACHE>::Read reads a range inside the last partial "
    "64KiB block of the backing store like NO_CACHE does",
    "[file-reader][regression]")
{
  constexpr size_t kBlock = kCacheBlockSize;
  constexpr size_t kTail = kTailSize;

  std::vector<BYTE> backing(kBlock + kTail);
  for (size_t i = 0; i < backing.size(); i++)
  {
    backing[i] = static_cast<BYTE>(i * kPatternStepA + (i >> kBitsPerByte));
  }

  FileReader<Strategy::FULL_CACHE> full(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));
  FileReader<Strategy::NO_CACHE> exact(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));

  struct Range
  {
    size_t offset;
    DWORD length;
  };
  // Inside the partial block, ending exactly at the end, and straddling into
  // it from the full block before.
  const Range ranges[] = {{kBlock + kInnerOffset, kInnerLength},
                          {kBlock + kTail - kInnerLength, kInnerLength},
                          {kBlock - kStraddleBefore, kStraddleLength}};

  for (const Range& range : ranges)
  {
    LARGE_INTEGER addrFull{.QuadPart = gsl::narrow<LONGLONG>(range.offset)};
    LARGE_INTEGER addrExact{.QuadPart = gsl::narrow<LONGLONG>(range.offset)};

    const auto expected = exact.Read(addrExact, range.length);
    REQUIRE(expected.has_value());

    const auto actual = full.Read(addrFull, range.length);
    REQUIRE(actual.has_value());
    REQUIRE(actual->size() == expected->size());
    CHECK(std::equal(actual->begin(), actual->end(), expected->begin()));
    CHECK(std::equal(actual->begin(), actual->end(),
                     backing.begin() + gsl::narrow<ptrdiff_t>(range.offset)));
  }

  // Reading the same range twice keeps returning the same bytes.
  LARGE_INTEGER again{.QuadPart = static_cast<LONGLONG>(kBlock + kInnerOffset)};
  const auto second = full.Read(again, kInnerLength);
  REQUIRE(second.has_value());
  CHECK(std::equal(second->begin(), second->end(),
                   backing.begin() +
                       static_cast<ptrdiff_t>(kBlock + kInnerOffset)));

  // A range that really runs past the end still fails, as in NO_CACHE.
  LARGE_INTEGER pastEnd{
      .QuadPart = static_cast<LONGLONG>(kBlock + kTail - kPastEndBefore)};
  CHECK_FALSE(full.Read(pastEnd, kInnerLength).has_value());
}

TEST_CASE(
    "FileReader<FULL_CACHE>::Read reads from a backing store smaller than "
    "one 64KiB block",
    "[file-reader][regression]")
{
  std::vector<BYTE> backing(kTailSize);
  for (size_t i = 0; i < backing.size(); i++)
  {
    backing[i] = static_cast<BYTE>(i * kPatternStepB);
  }

  FileReader<Strategy::FULL_CACHE> full(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));

  LARGE_INTEGER addr{.QuadPart = kInnerLength};
  const auto result = full.Read(addr, kInnerLength);
  REQUIRE(result.has_value());
  REQUIRE(result->size() == kInnerLength);
  CHECK(std::equal(result->begin(), result->end(),
                   backing.begin() + kInnerLength));
}

TEST_CASE(
    "FileReader<FULL_CACHE>::Read rejects a negative address instead of "
    "returning a view before its cache block",
    "[file-reader][regression]")
{
  std::vector<BYTE> backing(kCacheBlockSize);
  FileReader<Strategy::FULL_CACHE> full(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));

  LARGE_INTEGER addr{.QuadPart = -1};
  CHECK_FALSE(full.Read(addr, 100).has_value());
}

TEST_CASE(
    "FileReader<FULL_CACHE>::Read rejects a range that runs past the largest "
    "signed address",
    "[file-reader][regression]")
{
  std::vector<BYTE> backing(kCacheBlockSize);
  FileReader<Strategy::FULL_CACHE> full(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));

  constexpr DWORD kLength = 100;
  const LONGLONG addresses[] = {std::numeric_limits<LONGLONG>::max(),
                                std::numeric_limits<LONGLONG>::max() - 10,
                                std::numeric_limits<LONGLONG>::max() -
                                    (kLength - 1)};
  for (const LONGLONG address : addresses)
  {
    INFO("address " << address);
    LARGE_INTEGER addr{.QuadPart = address};
    CHECK_FALSE(full.Read(addr, kLength).has_value());
  }
}
