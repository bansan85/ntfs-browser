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

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/strategy.h>

#include "file-reader.h"
#include "memory-disk-reader.h"

using NtfsBrowser::FileReader;
using NtfsBrowser::Strategy;

TEST_CASE(
    "FileReader<FULL_CACHE>::Read must not abort/misbehave on a read "
    "crossing a 64KiB cache block boundary",
    "[file-reader][regression]")
{
  // Each byte distinct from its offset, so returned data can be checked
  // exactly.
  std::vector<BYTE> backing(2 * 65536);
  for (size_t i = 0; i < backing.size(); i++)
  {
    backing[i] = static_cast<BYTE>(i);
  }

  auto reader_double =
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing);
  FileReader<Strategy::FULL_CACHE> reader(std::move(reader_double));

  // Straddles the boundary between the first and second 64KiB cache blocks.
  LARGE_INTEGER addr{.QuadPart = 65536 - 36};
  constexpr DWORD kLength = 100;

  // A crossing read may abort the process; ctest reports that as this
  // test failing.
  const std::optional<std::span<const BYTE>> result =
      reader.Read(addr, kLength);

  if (result)
  {
    CHECK(result->size() == kLength);
    for (size_t i = 0; i < result->size(); i++)
    {
      CHECK((*result)[i] == backing[65536 - 36 + i]);
    }
  }
}

TEST_CASE(
    "FileReader<FULL_CACHE>::Read reads a range inside the last partial "
    "64KiB block of the backing store like NO_CACHE does",
    "[file-reader][regression]")
{
  constexpr size_t kBlock = 65536;
  constexpr size_t kTail = 4096;

  std::vector<BYTE> backing(kBlock + kTail);
  for (size_t i = 0; i < backing.size(); i++)
  {
    backing[i] = static_cast<BYTE>(i * 7 + (i >> 8));
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
  const Range ranges[] = {
      {kBlock + 1024, 512}, {kBlock + kTail - 512, 512}, {kBlock - 36, 100}};

  for (const Range& range : ranges)
  {
    LARGE_INTEGER addrFull{.QuadPart = static_cast<LONGLONG>(range.offset)};
    LARGE_INTEGER addrExact{.QuadPart = static_cast<LONGLONG>(range.offset)};

    const auto expected = exact.Read(addrExact, range.length);
    REQUIRE(expected.has_value());

    const auto actual = full.Read(addrFull, range.length);
    REQUIRE(actual.has_value());
    REQUIRE(actual->size() == expected->size());
    CHECK(std::equal(actual->begin(), actual->end(), expected->begin()));
    CHECK(std::equal(actual->begin(), actual->end(),
                     backing.begin() + static_cast<ptrdiff_t>(range.offset)));
  }

  // Reading the same range twice keeps returning the same bytes.
  LARGE_INTEGER again{.QuadPart = static_cast<LONGLONG>(kBlock + 1024)};
  const auto second = full.Read(again, 512);
  REQUIRE(second.has_value());
  CHECK(std::equal(second->begin(), second->end(),
                   backing.begin() + static_cast<ptrdiff_t>(kBlock + 1024)));

  // A range that really runs past the end still fails, as in NO_CACHE.
  LARGE_INTEGER pastEnd{.QuadPart =
                            static_cast<LONGLONG>(kBlock + kTail - 256)};
  CHECK_FALSE(full.Read(pastEnd, 512).has_value());
}

TEST_CASE(
    "FileReader<FULL_CACHE>::Read reads from a backing store smaller than "
    "one 64KiB block",
    "[file-reader][regression]")
{
  std::vector<BYTE> backing(4096);
  for (size_t i = 0; i < backing.size(); i++)
  {
    backing[i] = static_cast<BYTE>(i * 3);
  }

  FileReader<Strategy::FULL_CACHE> full(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(backing));

  LARGE_INTEGER addr{.QuadPart = 512};
  const auto result = full.Read(addr, 512);
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 512);
  CHECK(std::equal(result->begin(), result->end(), backing.begin() + 512));
}

TEST_CASE(
    "FileReader<FULL_CACHE>::Read rejects a negative address instead of "
    "returning a view before its cache block",
    "[file-reader][regression]")
{
  std::vector<BYTE> backing(65536);
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
  std::vector<BYTE> backing(65536);
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
