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
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "io/file-reader.h"
#include "memory-disk-reader.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using FullReader =
    NTFS_BROWSER_NAMESPACE::Io::FileReader<Cache::Strategy::FullCache>;

namespace {

// Bytes of one block of FileReader<FullCache>.
constexpr std::uint64_t ceiling_block = 65536;

// Blocks of the backing store.
constexpr std::size_t ceiling_backing_blocks = 4;

// Length of the reads: small, and inside one block.
constexpr DWORD ceiling_read_length = 64;

// Serves a memory image and counts the reads that reach it.
class CountingDiskReader final : public NTFS_BROWSER_NAMESPACE::IDiskReader {
 public:
  CountingDiskReader(std::vector<BYTE> data, std::shared_ptr<int> reads)
      : inner_(std::move(data)), reads_(std::move(reads)) {}

  bool Open(std::wstring_view path) override { return inner_.Open(path); }

  [[nodiscard]] bool ReadInto(LARGE_INTEGER& addr,
                              std::span<BYTE> dest) const override {
    ++*reads_;
    return inner_.ReadInto(addr, dest);
  }

 private:
  NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader inner_;
  std::shared_ptr<int> reads_;
};

// A reader over a backing store of ceiling_backing_blocks blocks, with the
// given ceiling. "reads" counts the disk reads it makes.
struct CountedReader {
  std::shared_ptr<int> reads = std::make_shared<int>(0);
  FullReader reader;

  explicit CountedReader(std::uint64_t ceiling)
      : reader(std::make_unique<CountingDiskReader>(
                   std::vector<BYTE>(ceiling_backing_blocks * ceiling_block,
                                     BYTE{0x42}),
                   reads),
               ceiling) {}

  // Reads from block "block" and drops the view at once.
  void Touch(std::uint64_t block) const {
    LARGE_INTEGER addr{.QuadPart =
                           static_cast<LONGLONG>(block * ceiling_block)};
    REQUIRE(reader.Read(addr, ceiling_read_length).has_value());
  }
};

}  // namespace

TEST_CASE("Ceiling 0 frees a block as soon as its last view is dropped",
          "[cache][ceiling]") {
  const CountedReader counted(0);
  LARGE_INTEGER addr{.QuadPart = 0};

  std::optional<Cache::View> view =
      counted.reader.Read(addr, ceiling_read_length);
  REQUIRE(view.has_value());

  CHECK(counted.reader.GetCacheStats().held_bytes == ceiling_block);
  CHECK(std::ranges::all_of(std::span<const BYTE>(*view),
                            [](BYTE byte) { return byte == 0x42; }));

  view.reset();

  CHECK(counted.reader.GetCacheStats().held_bytes == 0);
}

TEST_CASE("A very small ceiling behaves like ceiling 0", "[cache][ceiling]") {
  const CountedReader counted(1);

  counted.Touch(0);
  counted.Touch(0);

  CHECK(counted.reader.GetCacheStats().held_bytes == 0);
  // Nothing stays cached, so each touch reads the disk.
  CHECK(*counted.reads == 2);
}

TEST_CASE("Unlimited ceiling never evicts", "[cache][ceiling]") {
  const CountedReader counted(Cache::unlimited_bytes);

  for (int pass = 0; pass < 3; pass++) {
    for (std::uint64_t block = 0; block < ceiling_backing_blocks; block++) {
      counted.Touch(block);
    }
  }

  CHECK(counted.reader.GetCacheStats().held_bytes ==
        ceiling_backing_blocks * ceiling_block);
  // One disk read per block, whatever the number of passes.
  CHECK(*counted.reads == static_cast<int>(ceiling_backing_blocks));
}

TEST_CASE(
    "A ceiling between the extremes evicts the least recently used "
    "block first",
    "[cache][ceiling]") {
  const CountedReader counted(2 * ceiling_block);
  counted.Touch(0);
  counted.Touch(1);
  // Block 0 is the oldest use, so block 2 pushes it out.
  counted.Touch(2);
  CHECK(counted.reader.GetCacheStats().held_bytes == 2 * ceiling_block);
  REQUIRE(*counted.reads == 3);

  counted.Touch(1);
  counted.Touch(2);
  CHECK(*counted.reads == 3);

  counted.Touch(0);
  CHECK(*counted.reads == 4);
}

TEST_CASE("A read of cached data counts as a use", "[cache][ceiling]") {
  const CountedReader counted(2 * ceiling_block);
  counted.Touch(0);
  counted.Touch(1);
  // Block 0 becomes the newest, so block 1 is the one block 2 pushes out.
  counted.Touch(0);
  counted.Touch(2);
  REQUIRE(*counted.reads == 3);

  counted.Touch(0);
  CHECK(*counted.reads == 3);

  counted.Touch(1);
  CHECK(*counted.reads == 4);
}
