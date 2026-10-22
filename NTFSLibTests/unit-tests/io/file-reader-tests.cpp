#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/cache/stats.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/cache/view.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "catch2/catch_message.hpp"
#include "io/file-reader.h"
#include "memory-disk-reader.h"
#include "optional-access.h"
#include "partition-disk-reader.h"

namespace {

// Size of the backing file the read tests fill with a byte pattern.
constexpr size_t content_size = 4096;

// Multiplier of the byte pattern, so a shifted read cannot match by accident.
constexpr size_t pattern_step = 7;

// Where, and how much, the ReadInto test reads.
constexpr size_t read_into_offset = 1000;
constexpr size_t read_into_size = 128;

// A file too small for the read asked of it, and its fill byte.
constexpr size_t tiny_file_size = 16;
constexpr BYTE tiny_file_fill = 0xAB;

// The two reads of the buffer-growth test: a small one, then a larger one at
// another offset.
constexpr DWORD first_read_size = 16;
constexpr LONGLONG second_read_offset = 100;
constexpr DWORD second_read_size = 2048;

// Opens path through a PartitionDiskReader (offset 0) instead of
// FileReader's own Open(), which only exists on Windows (Win32DiskReader).
template <NTFS_BROWSER_NAMESPACE::Cache::Strategy S>
NTFS_BROWSER_NAMESPACE::Io::FileReader<S>
    OpenOnDisk(const std::filesystem::path& path) {
  auto reader =
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::PartitionDiskReader>(0);
  REQUIRE(reader->Open(path.wstring()));
  return NTFS_BROWSER_NAMESPACE::Io::FileReader<S>(std::move(reader));
}

// Writes content to a new temp file and returns its path.
std::filesystem::path WriteTempFile(std::span<const BYTE> content) {
  std::random_device random_device;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-reader-test-" + std::to_wstring(random_device()) +
       L".bin");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(content.data()),
            gsl::narrow<std::streamsize>(content.size()));

  return path;
}

struct TempFile final {
  std::filesystem::path path;

  explicit TempFile(std::span<const BYTE> content)
      : path(WriteTempFile(content)) {}

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;

  ~TempFile() { std::filesystem::remove(path); }
};

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileReader::ReadInto reads into the caller-provided buffer",
    "[file-reader]", ((NTFS_BROWSER_NAMESPACE::Cache::Strategy S), S),
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache,
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache) {
  std::vector<BYTE> content(content_size);
  for (size_t i = 0; i < content.size(); i++) {
    // i < content.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    content[i] = static_cast<BYTE>(i);
  }
  const TempFile file(content);

  const NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader =
      OpenOnDisk<S>(file.path);

  std::array<BYTE, read_into_size> dest{};
  LARGE_INTEGER addr{.QuadPart = read_into_offset};
  REQUIRE(reader.ReadInto(addr, dest));

  for (size_t i = 0; i < dest.size(); i++) {
    // i < dest.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(dest[i] == content.at(read_into_offset + i));
  }
}

TEMPLATE_TEST_CASE_SIG("FileReader::ReadInto fails past end of file",
                       "[file-reader]",
                       ((NTFS_BROWSER_NAMESPACE::Cache::Strategy S), S),
                       NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache,
                       NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache) {
  std::vector<BYTE> content(tiny_file_size, tiny_file_fill);
  const TempFile file(content);

  const NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader =
      OpenOnDisk<S>(file.path);

  std::array<BYTE, read_into_size> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("FileReader NoCache Read grows its buffer before filling it",
          "[file-reader]") {
  std::vector<BYTE> content(content_size);
  for (size_t i = 0; i < content.size(); i++) {
    // i < content.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    content[i] = static_cast<BYTE>(i * pattern_step);
  }

  const NTFS_BROWSER_NAMESPACE::Io::FileReader<
      NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>
      reader(std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          content));

  LARGE_INTEGER first_addr{.QuadPart = 0};
  const auto first = reader.Read(first_addr, first_read_size);
  REQUIRE(first.has_value());
  REQUIRE(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(first).size() ==
          first_read_size);

  LARGE_INTEGER second_addr{.QuadPart = second_read_offset};
  const auto second = reader.Read(second_addr, second_read_size);
  REQUIRE(second.has_value());
  REQUIRE(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(second).size() ==
          second_read_size);
  for (size_t i = 0; i < NTFS_BROWSER_NAMESPACE::Tests::Unwrap(second).size();
       i++) {
    // i < second->size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    REQUIRE(NTFS_BROWSER_NAMESPACE::Tests::Unwrap(second)[i] ==
            content.at(second_read_offset + i));
  }
}

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::Io::FileReader;

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
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          backing);
  const FileReader<Cache::Strategy::FullCache> reader(std::move(reader_double));

  // Straddles the boundary between the first and second 64KiB cache blocks.
  LARGE_INTEGER addr{.QuadPart = cache_block_size - straddle_before};
  constexpr DWORD length_value = straddle_length;

  // A crossing read may abort the process; ctest reports that as this
  // test failing.
  const std::optional<Cache::View> result = reader.Read(addr, length_value);

  if (result) {
    const std::span<const BYTE> bytes = *result;
    CHECK(bytes.size() == length_value);
    for (size_t i = 0; i < bytes.size(); i++) {
      // i < bytes.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      CHECK(bytes[i] == backing.at(cache_block_size - straddle_before + i));
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
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          backing));
  const FileReader<Cache::Strategy::NoCache> exact(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          backing));

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

    const auto actual_view = full.Read(addr_full, range.length);
    REQUIRE(actual_view.has_value());
    const std::span<const BYTE> actual =
        NTFS_BROWSER_NAMESPACE::Tests::Unwrap(actual_view);
    REQUIRE(actual.size() ==
            NTFS_BROWSER_NAMESPACE::Tests::Unwrap(expected).size());
    CHECK(std::equal(actual.begin(), actual.end(),
                     NTFS_BROWSER_NAMESPACE::Tests::Unwrap(expected).begin()));
    CHECK(std::equal(actual.begin(), actual.end(),
                     backing.begin() + gsl::narrow<ptrdiff_t>(range.offset)));
  }

  // Reading the same range twice keeps returning the same bytes.
  LARGE_INTEGER again{.QuadPart =
                          static_cast<LONGLONG>(block_value + inner_offset)};
  const auto second_view = full.Read(again, inner_length);
  REQUIRE(second_view.has_value());
  const std::span<const BYTE> second =
      NTFS_BROWSER_NAMESPACE::Tests::Unwrap(second_view);
  CHECK(std::equal(second.begin(), second.end(),
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
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          backing));

  LARGE_INTEGER addr{.QuadPart = inner_length};
  const auto result_view = full.Read(addr, inner_length);
  REQUIRE(result_view.has_value());
  const std::span<const BYTE> result =
      NTFS_BROWSER_NAMESPACE::Tests::Unwrap(result_view);
  REQUIRE(result.size() == inner_length);
  CHECK(
      std::equal(result.begin(), result.end(), backing.begin() + inner_length));
}

TEST_CASE(
    "FileReader<FullCache>::Read rejects a negative address instead of "
    "returning a view before its cache block",
    "[file-reader][regression]") {
  const std::vector<BYTE> backing(cache_block_size);
  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          backing));

  LARGE_INTEGER addr{.QuadPart = -1};
  CHECK_FALSE(full.Read(addr, 100).has_value());
}

TEST_CASE(
    "FileReader<FullCache>::Read rejects a range that runs past the largest "
    "signed address",
    "[file-reader][regression]") {
  const std::vector<BYTE> backing(cache_block_size);
  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          backing));

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

namespace {

// Blocks of the backing store of the bounded-cache tests: enough for a read
// that spans three of them, plus a fourth.
constexpr size_t bounded_backing_blocks = 4;

// Serves a memory image, and fails every read while "failing" is set.
class SwitchableDiskReader final : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  SwitchableDiskReader(std::vector<BYTE> data, std::shared_ptr<bool> failing)
      : inner_(std::move(data)), failing_(std::move(failing)) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    return !*failing_ && inner_.ReadInto(addr, dest);
  }

 private:
  NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader inner_;
  std::shared_ptr<bool> failing_;
};

// Bytes that depend on their offset, so a view over the wrong range cannot
// match by accident.
std::vector<BYTE> BoundedBacking() {
  std::vector<BYTE> backing(bounded_backing_blocks * cache_block_size);
  for (size_t i = 0; i < backing.size(); i++) {
    // i < backing.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    backing[i] = static_cast<BYTE>(i * pattern_step_a + (i >> bits_per_byte));
  }
  return backing;
}

// True when the bytes are the backing store's at "offset".
bool MatchesBounded(std::span<const BYTE> bytes, size_t offset) {
  const std::vector<BYTE> backing = BoundedBacking();
  return std::equal(bytes.begin(), bytes.end(),
                    backing.begin() + gsl::narrow<ptrdiff_t>(offset));
}

}  // namespace

TEST_CASE(
    "FileReader<FullCache>::Read caches each missing block of a straddling "
    "read and returns the bytes NoCache returns",
    "[file-reader][cache]") {
  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          BoundedBacking()),
      Cache::unlimited_bytes);
  const FileReader<Cache::Strategy::NoCache> exact(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          BoundedBacking()));

  LARGE_INTEGER addr_full{.QuadPart = cache_block_size - straddle_before};
  LARGE_INTEGER addr_exact{.QuadPart = cache_block_size - straddle_before};
  {
    const auto actual = full.Read(addr_full, straddle_length);
    const auto expected = exact.Read(addr_exact, straddle_length);
    REQUIRE(actual.has_value());
    REQUIRE(expected.has_value());
    const std::span<const BYTE> actual_bytes = *actual;
    const std::span<const BYTE> expected_bytes = *expected;
    CHECK(std::ranges::equal(actual_bytes, expected_bytes));
    // The stitched copy belongs to the view, apart from the two blocks.
    CHECK(full.GetCacheStats().held_bytes ==
          (2 * cache_block_size) + straddle_length);
    CHECK(full.GetCacheStats().in_use_bytes == straddle_length);
  }

  CHECK(full.GetCacheStats().held_bytes == 2 * cache_block_size);
  CHECK(full.GetCacheStats().in_use_bytes == 0);
}

TEST_CASE("FileReader<FullCache>::Read serves a read larger than the ceiling",
          "[file-reader][cache]") {
  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          BoundedBacking()),
      cache_block_size);

  constexpr size_t offset = 100;
  constexpr DWORD length = (3 * cache_block_size) - (2 * offset);
  LARGE_INTEGER addr{.QuadPart = offset};
  {
    const auto result = full.Read(addr, length);
    REQUIRE(result.has_value());
    CHECK(result->size() == length);
    CHECK(MatchesBounded(*result, offset));
    // The view owns a copy larger than the ceiling, so it is all in use.
    CHECK(full.GetCacheStats().in_use_bytes == length);
  }

  CHECK(full.GetCacheStats().held_bytes <= cache_block_size);
}

TEST_CASE(
    "FileReader<FullCache>::Read reports a failed read, caches nothing of "
    "it and reads correctly afterwards",
    "[file-reader][cache]") {
  const auto failing = std::make_shared<bool>(false);
  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<SwitchableDiskReader>(BoundedBacking(), failing),
      cache_block_size);

  const auto read_block = [&full](size_t block, DWORD length) {
    LARGE_INTEGER addr{.QuadPart =
                           gsl::narrow<LONGLONG>(block * cache_block_size)};
    return full.Read(addr, length);
  };
  REQUIRE(read_block(0, inner_length).has_value());
  REQUIRE(read_block(1, inner_length).has_value());
  const uint64_t held_before = full.GetCacheStats().held_bytes;
  REQUIRE(held_before == cache_block_size);

  *failing = true;
  CHECK_FALSE(read_block(2, inner_length).has_value());
  // A straddling read fails too, and its stitched buffer goes with it.
  LARGE_INTEGER straddle{.QuadPart = (3 * cache_block_size) - straddle_before};
  CHECK_FALSE(full.Read(straddle, straddle_length).has_value());
  CHECK(full.GetCacheStats().held_bytes == held_before);

  *failing = false;
  const auto again = read_block(2, inner_length);
  REQUIRE(again.has_value());
  CHECK(MatchesBounded(*again, 2 * cache_block_size));
}

TEST_CASE(
    "FileReader<FullCache>::Read leaves the data under a held view unchanged "
    "while other reads evict blocks",
    "[file-reader][cache]") {
  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          BoundedBacking()),
      0);

  LARGE_INTEGER held_addr{.QuadPart = inner_offset};
  const auto held = full.Read(held_addr, inner_length);
  REQUIRE(held.has_value());

  for (int pass = 0; pass < 3; pass++) {
    for (size_t block = 1; block < bounded_backing_blocks; block++) {
      LARGE_INTEGER addr{.QuadPart =
                             gsl::narrow<LONGLONG>(block * cache_block_size)};
      REQUIRE(full.Read(addr, inner_length).has_value());
    }
  }

  CHECK(MatchesBounded(*held, inner_offset));
  CHECK(full.GetCacheStats().held_bytes == cache_block_size);
}

TEST_CASE("FileReader<FullCache>::Minify frees only the blocks no view uses",
          "[file-reader][cache]") {
  const FileReader<Cache::Strategy::FullCache> full(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          BoundedBacking()),
      Cache::unlimited_bytes);

  LARGE_INTEGER held_addr{.QuadPart = 0};
  const auto held = full.Read(held_addr, inner_length);
  REQUIRE(held.has_value());
  LARGE_INTEGER other_addr{.QuadPart = cache_block_size};
  REQUIRE(full.Read(other_addr, inner_length).has_value());
  REQUIRE(full.GetCacheStats().held_bytes == 2 * cache_block_size);

  full.Minify();

  CHECK(full.GetCacheStats().held_bytes == cache_block_size);
  CHECK(full.GetCacheStats().in_use_bytes == cache_block_size);
  CHECK(MatchesBounded(*held, 0));
}

TEST_CASE("FileReader<NoCache> keeps no cache to report or minify",
          "[file-reader][cache]") {
  const FileReader<Cache::Strategy::NoCache> exact(
      std::make_unique<NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader>(
          BoundedBacking()));
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(exact.Read(addr, inner_length).has_value());

  exact.Minify();

  CHECK(exact.GetCacheStats().held_bytes == 0);
  CHECK(exact.GetCacheStats().in_use_bytes == 0);
}

#ifdef _WIN32
TEMPLATE_TEST_CASE_SIG(
    "FileReader::Open honours the length of a non NUL-terminated view",
    "[file-reader]", ((NTFS_BROWSER_NAMESPACE::Cache::Strategy S), S),
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache,
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache) {
  const std::vector<BYTE> content(64, 0x5A);
  TempFile file(content);

  const std::wstring real_path = file.path.wstring();
  const std::wstring longer = real_path + L"xyz";

  NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader;
  REQUIRE(reader.Open(std::wstring_view(longer.data(), real_path.size())));

  std::array<BYTE, tiny_file_size> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(reader.ReadInto(addr, dest));
  // dest holds tiny_file_size = 16 bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(dest[0] == 0x5A);
}
#endif

TEMPLATE_TEST_CASE_SIG("A FileReader with no reader ignores a read ahead",
                       "[file-reader]",
                       ((NTFS_BROWSER_NAMESPACE::Cache::Strategy S), S),
                       NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache,
                       NTFS_BROWSER_NAMESPACE::Cache::Strategy::FullCache) {
  NTFS_BROWSER_NAMESPACE::Io::FileReader<S> reader;

  reader.EnableReadAhead(first_read_size);

  CHECK_FALSE(reader.Size().has_value());
}
