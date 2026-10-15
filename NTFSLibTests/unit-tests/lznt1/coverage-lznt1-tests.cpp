#include <ntfs-browser/win-types.h>

#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "lznt1/decompress.h"

TEST_CASE("LZNT1 rejects compressed output that overflows its bounds",
          "[lznt1][cov-nri]") {
  SECTION("a back-reference longer than the destination's room") {
    // Literal 'A', then a back-reference of length 3 into a 2-byte buffer.
    const std::vector<BYTE> src{0x03, 0xB0, 0x02, 'A', 0x00, 0x00};
    std::vector<BYTE> dest(2);
    CHECK_THROWS_AS(NTFS_BROWSER_NAMESPACE::Lznt1::Decompress(src, dest),
                    std::runtime_error);
  }

  SECTION("a literal past the end of the destination") {
    // Two literals, 'A' and 'B', into a 1-byte buffer.
    const std::vector<BYTE> src{0x02, 0xB0, 0x00, 'A', 'B'};
    std::vector<BYTE> dest(1);
    CHECK_THROWS_AS(NTFS_BROWSER_NAMESPACE::Lznt1::Decompress(src, dest),
                    std::runtime_error);
  }

  SECTION("elements that decode past 4096 bytes in one chunk") {
    // A 4095-byte back-reference fills the chunk, then one more literal
    // arrives.
    const std::vector<BYTE> src{0x05, 0xB0, 0x02, 'A', 0xFC, 0x0F, 'B', 'C'};
    std::vector<BYTE> dest(2 * NTFS_BROWSER_NAMESPACE::Lznt1::chunk_size);
    CHECK_THROWS_AS(NTFS_BROWSER_NAMESPACE::Lznt1::Decompress(src, dest),
                    std::runtime_error);
  }
}
