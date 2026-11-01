#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tuning/tuning.h>

#include "counting-disk-reader.h"
#include "io/file-reader.h"
#include "memory-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::Io::FileReader;

namespace {

// Bytes of one cache block: the default of the tuning.
constexpr ULONGLONG block = Tuning::Settings{}.cache.block_bytes;

// Blocks of the backing store of the run tests.
constexpr ULONGLONG store_blocks = 16;

// Offset of the byte at "i" in the pattern: a misplaced read cannot match.
constexpr size_t pattern_step = 7;

std::vector<BYTE> Pattern(size_t size) {
  std::vector<BYTE> bytes(size);
  for (size_t i = 0; i < bytes.size(); i++) {
    // i < bytes.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    bytes[i] = static_cast<BYTE>((i * pattern_step) + (i >> 8U));
  }
  return bytes;
}

// A FullCache FileReader over a pattern of "blocks" blocks and "tail" more
// bytes, with the calls it makes counted.
struct CountedReader {
  CountedReader(ULONGLONG blocks, ULONGLONG tail, std::uint64_t ceiling,
                const Tuning::Settings& tuning = {})
      : bytes(Pattern(static_cast<size_t>((blocks * block) + tail))) {
    auto counting = std::make_unique<Tests::CountingDiskReader>(
        std::make_unique<Tests::MemoryDiskReader>(bytes));
    calls = counting.get();
    reader = std::make_unique<FileReader<Cache::Strategy::FullCache>>(
        std::move(counting), ceiling, tuning);
  }

  // The result of a read, true when it holds the pattern at "offset".
  [[nodiscard]] bool ReadMatches(ULONGLONG offset, DWORD length) const {
    LARGE_INTEGER addr{.QuadPart = gsl::narrow<LONGLONG>(offset)};
    const std::optional<Cache::View> view = reader->Read(addr, length);
    if (!view) {
      return false;
    }
    const std::span<const BYTE> got = *view;
    return got.size() == length &&
           std::equal(got.begin(), got.end(),
                      bytes.begin() + gsl::narrow<ptrdiff_t>(offset));
  }

  // The (offset, length) of every read so far, in order.
  [[nodiscard]] std::vector<std::pair<ULONGLONG, ULONGLONG>> Reads() const {
    std::vector<std::pair<ULONGLONG, ULONGLONG>> reads;
    for (const Tests::CountingDiskReader::Call& call : calls->Calls()) {
      if (call.kind == Tests::CountingDiskReader::Kind::Read) {
        reads.emplace_back(call.offset, call.length);
      }
    }
    return reads;
  }

  std::vector<BYTE> bytes;
  Tests::CountingDiskReader* calls{nullptr};
  std::unique_ptr<FileReader<Cache::Strategy::FullCache>> reader;
};

using Reads = std::vector<std::pair<ULONGLONG, ULONGLONG>>;

// Settings with the cap of a run set.
Tuning::Settings RunCap(std::size_t bytes) {
  Tuning::Settings tuning;
  tuning.cache.max_run_bytes = bytes;
  return tuning;
}

}  // namespace

TEST_CASE("A FullCache read that spans blocks reads the missing ones at once",
          "[file-reader][cache][run]") {
  const CountedReader counted(store_blocks, 0, Cache::unlimited_bytes);

  REQUIRE(counted.ReadMatches(0, gsl::narrow<DWORD>(store_blocks * block)));

  CHECK(counted.Reads() == Reads{{0, store_blocks * block}});
  // Every block of the run is cached as a block of its own.
  CHECK(counted.reader->GetCacheStats().held_bytes == store_blocks * block);

  counted.calls->Reset();
  REQUIRE(counted.ReadMatches(block + 5, gsl::narrow<DWORD>(3 * block)));
  CHECK(counted.Reads().empty());
}

TEST_CASE("A run of missing blocks ends at a block the cache holds",
          "[file-reader][cache][run]") {
  const CountedReader counted(store_blocks, 0, Cache::unlimited_bytes);
  REQUIRE(counted.ReadMatches(5 * block + 10, 100));
  counted.calls->Reset();

  REQUIRE(counted.ReadMatches(2 * block, gsl::narrow<DWORD>(8 * block)));

  CHECK(counted.Reads() ==
        Reads{{2 * block, 3 * block}, {6 * block, 4 * block}});
}

TEST_CASE("A run reads the blocks of the range and no more",
          "[file-reader][cache][run]") {
  const CountedReader counted(store_blocks, 0, Cache::unlimited_bytes);

  // From the middle of block 3 to the middle of block 5.
  REQUIRE(counted.ReadMatches((3 * block) + 100,
                              gsl::narrow<DWORD>((2 * block) + 50)));

  CHECK(counted.Reads() == Reads{{3 * block, 3 * block}});
}

TEST_CASE("A run is cut at the cap of the tuning",
          "[file-reader][cache][run]") {
  const CountedReader counted(store_blocks, 0, Cache::unlimited_bytes,
                              RunCap(4 * block));

  REQUIRE(counted.ReadMatches(0, gsl::narrow<DWORD>(10 * block)));

  CHECK(counted.Reads() ==
        Reads{{0, 4 * block}, {4 * block, 4 * block}, {8 * block, 2 * block}});
}

TEST_CASE("A cap under one block reads one block at a time",
          "[file-reader][cache][run]") {
  const CountedReader counted(store_blocks, 0, Cache::unlimited_bytes,
                              RunCap(1));

  REQUIRE(counted.ReadMatches(0, gsl::narrow<DWORD>(3 * block)));

  CHECK(counted.Reads() ==
        Reads{{0, block}, {block, block}, {2 * block, block}});
}

TEST_CASE("A read inside one block reads that block only",
          "[file-reader][cache][run]") {
  const CountedReader counted(store_blocks, 0, Cache::unlimited_bytes);

  REQUIRE(counted.ReadMatches((2 * block) + 10, 100));

  CHECK(counted.Reads() == Reads{{2 * block, block}});
}

TEST_CASE("A cache that keeps nothing still reads a run once",
          "[file-reader][cache][run]") {
  const CountedReader counted(store_blocks, 0, 0);

  REQUIRE(counted.ReadMatches(100, gsl::narrow<DWORD>(8 * block)));

  // Nine blocks hold the range, and none is read twice.
  CHECK(counted.Reads() == Reads{{0, 9 * block}});
  CHECK(counted.reader->GetCacheStats().held_bytes == 0);
}

TEST_CASE("A run that would pass the end of the store falls back to blocks",
          "[file-reader][cache][run]") {
  // The store ends in the middle of its last block.
  const CountedReader counted(4, block / 2, Cache::unlimited_bytes);

  // The whole range is served, as a block alone cannot be read to its end.
  REQUIRE(counted.ReadMatches(2 * block,
                              gsl::narrow<DWORD>((2 * block) + (block / 2))));

  // Nothing partial stays in the cache: the blocks before the last one do.
  CHECK(counted.reader->GetCacheStats().held_bytes == 2 * block);
}

TEST_CASE("A failed run is read again block by block, then served",
          "[file-reader][cache][run]") {
  const CountedReader counted(store_blocks, 0, Cache::unlimited_bytes);
  // The third block of the run cannot be read, the others can.
  counted.calls->FailReadsIn(2 * block, 3 * block);

  LARGE_INTEGER addr{.QuadPart = 0};
  CHECK_FALSE(
      counted.reader->Read(addr, gsl::narrow<DWORD>(4 * block)).has_value());
  // The run failed, the blocks were read apart up to the bad one, and the
  // range was then tried whole.
  CHECK(counted.Reads() == Reads{{0, 4 * block},
                                 {0, block},
                                 {block, block},
                                 {2 * block, block},
                                 {0, 4 * block}});
  // Only the blocks read whole are cached.
  CHECK(counted.reader->GetCacheStats().held_bytes == 2 * block);
}

TEST_CASE("The cap of a run never goes under a block",
          "[file-reader][cache][run]") {
  Tuning::Settings tuning = RunCap(10);
  tuning.cache.block_bytes = 2 * block;
  const CountedReader counted(store_blocks, 0, Cache::unlimited_bytes, tuning);

  REQUIRE(counted.ReadMatches(0, gsl::narrow<DWORD>(4 * block)));

  CHECK(counted.Reads() == Reads{{0, 2 * block}, {2 * block, 2 * block}});
}

TEST_CASE("the CPU cost of a large read through the block cache",
          "[.benchmark][file-reader][cache][run]") {
  constexpr ULONGLONG read_bytes = ULONGLONG{32} * 1024 * 1024;
  constexpr int rounds = 5;
  const std::vector<BYTE> image = Pattern(static_cast<size_t>(read_bytes));

  struct Arm {
    const char* name;
    std::size_t block_bytes;
    std::size_t max_run_bytes;
  };

  const auto arms =
      std::to_array<Arm>({{"64 KiB blocks, one block a read", 65536, 65536},
                          {"4 KiB blocks, one block a read", 4096, 4096},
                          {"4 KiB blocks, runs of 64 KiB", 4096, 65536},
                          {"4 KiB blocks, runs of 1 MiB", 4096, 1024 * 1024}});
  for (const Arm& arm : arms) {
    Tuning::Settings tuning;
    tuning.cache.block_bytes = arm.block_bytes;
    tuning.cache.max_run_bytes = arm.max_run_bytes;
    std::vector<double> times;
    size_t reads = 0;
    for (int round = 0; round < rounds; round++) {
      auto counting = std::make_unique<Tests::CountingDiskReader>(
          std::make_unique<Tests::MemoryDiskReader>(image));
      Tests::CountingDiskReader* const calls = counting.get();
      const FileReader<Cache::Strategy::FullCache> reader(
          std::move(counting), Cache::default_max_cache_bytes, tuning);
      LARGE_INTEGER addr{.QuadPart = 0};
      const auto began = std::chrono::steady_clock::now();
      const std::optional<Cache::View> view =
          reader.Read(addr, gsl::narrow<DWORD>(read_bytes));
      times.push_back(std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - began)
                          .count());
      REQUIRE(view.has_value());
      REQUIRE(view->size() == read_bytes);
      reads = calls->ReadCount();
    }
    std::ranges::sort(times);
    // The median of the rounds, in milliseconds.
    WARN(arm.name << ": " << (times.at(rounds / 2) * 1000.0) << " ms, " << reads
                  << " reads");
  }
}
