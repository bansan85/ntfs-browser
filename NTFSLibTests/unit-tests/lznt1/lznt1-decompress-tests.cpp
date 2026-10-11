#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "compression-test-constants.h"
#include "fake-ntfs-image.h"
#include "lznt1/decompress.h"

namespace {

// Fills the bytes after End_of_buffer, which Decompress() must never read.
constexpr BYTE never_read_byte = 0xFF;

// A chunk bigger than the destination it is decompressed into, and that
// destination.
constexpr size_t oversized_chunk_size = 100;
constexpr size_t tiny_dest_size = 10;

}  // namespace

TEST_CASE("LZNT1 decompresses the [MS-XCA] section 3.3 worked example",
          "[lznt1][compression]") {
  std::vector<BYTE> out(NtfsBrowser::Lznt1::chunk_size, sentinel_byte);

  const size_t produced = NtfsBrowser::Lznt1::Decompress(
      NtfsBrowserTests::xca_lznt1_example_compressed, out);

  REQUIRE(produced == NtfsBrowserTests::xca_lznt1_example_decompressed_size);
  CHECK(std::memcmp(out.data(),
                    NtfsBrowserTests::xca_lznt1_example_decompressed.data(),
                    produced) == 0);
}

TEST_CASE("LZNT1 passes an uncompressed chunk through unchanged",
          "[lznt1][compression]") {
  const std::vector<BYTE> payload =
      NtfsBrowserTests::CompressionFixturePattern(1234);
  const std::vector<BYTE> chunk =
      NtfsBrowserTests::MakeUncompressedLznt1Chunk(payload);
  REQUIRE(chunk.size() == payload.size() + 2);

  std::vector<BYTE> out(NtfsBrowser::Lznt1::chunk_size, sentinel_byte);
  const size_t produced = NtfsBrowser::Lznt1::Decompress(chunk, out);

  REQUIRE(produced == payload.size());
  CHECK(std::memcmp(out.data(), payload.data(), payload.size()) == 0);
}

TEST_CASE("LZNT1 concatenates chunks and stops at End_of_buffer",
          "[lznt1][compression]") {
  const std::vector<BYTE> first =
      NtfsBrowserTests::CompressionFixturePattern(oversized_chunk_size);
  const std::vector<BYTE> second =
      NtfsBrowserTests::CompressionFixturePattern(50);

  std::vector<BYTE> src = NtfsBrowserTests::MakeUncompressedLznt1Chunk(first);
  const std::vector<BYTE> second_chunk =
      NtfsBrowserTests::MakeUncompressedLznt1Chunk(second);
  src.insert(src.end(), second_chunk.begin(), second_chunk.end());
  // End_of_buffer, followed by bytes that must never be looked at.
  src.insert(src.end(), {0x00, 0x00, never_read_byte, never_read_byte,
                         never_read_byte, never_read_byte});

  std::vector<BYTE> out(NtfsBrowser::Lznt1::chunk_size, sentinel_byte);
  const size_t produced = NtfsBrowser::Lznt1::Decompress(src, out);

  REQUIRE(produced == first.size() + second.size());
  CHECK(std::memcmp(out.data(), first.data(), first.size()) == 0);
  CHECK(std::memcmp(&out.at(first.size()), second.data(), second.size()) == 0);
}

TEST_CASE("LZNT1 rejects malformed input instead of reading out of bounds",
          "[lznt1][compression]") {
  std::vector<BYTE> out(NtfsBrowser::Lznt1::chunk_size, sentinel_byte);

  SECTION("chunk header with a wrong signature") {
    // Bits 14-12 must always be 3; here they are 0.
    const std::vector<BYTE> src{0x02, 0x80, 0x01, 0x00, 0x00};
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, out),
                    std::runtime_error);
  }

  SECTION("chunk declaring more data than the buffer holds") {
    // Declared chunk size 0x0FFF + 3 == 4098 bytes, but only 4 are here.
    const std::vector<BYTE> src{0xFF, 0xBF, 0x00, 0x00};
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, out),
                    std::runtime_error);
  }

  SECTION("compressed word truncated by the chunk's own declared size") {
    // Declared size 4 bytes = 2 payload bytes: a compressed-word flag, then
    // only 1 of its 2 bytes.
    const std::vector<BYTE> src{0x01, 0xB0, 0x01, 0x00};
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, out),
                    std::runtime_error);
  }

  SECTION("back-reference pointing before the start of the chunk") {
    // Same bytes BuildFakeNtfsImageWithCorruptCompressedUnit() writes:
    // displacement 1, nothing decompressed yet.
    const std::vector<BYTE> src{0x02, 0xB0, 0x01, 0x00, 0x00};
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, out),
                    std::runtime_error);
  }

  SECTION("chunk decompressing to more than 4096 bytes") {
    // Decodes to 4099 bytes: 1 over the 4096 per-chunk limit ([MS-XCA] 2.5.3).
    const std::vector<BYTE> src{0x03, 0xB0, 0x02, 0x41, 0xFF, 0x0F};
    // Roomy dest (2 chunks) so the per-chunk 4096 limit rejects this, not
    // the buffer bound.
    std::vector<BYTE> roomy(2 * NtfsBrowser::Lznt1::chunk_size, sentinel_byte);
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, roomy),
                    std::runtime_error);
  }

  SECTION("output overrunning the destination buffer") {
    // Well-formed 100-byte chunk into a 10-byte dest: a real unit never
    // exceeds its own size, so this is malformed.
    const std::vector<BYTE> src = NtfsBrowserTests::MakeUncompressedLznt1Chunk(
        NtfsBrowserTests::CompressionFixturePattern(oversized_chunk_size));
    std::vector<BYTE> tiny(tiny_dest_size, sentinel_byte);
    CHECK_THROWS_AS(NtfsBrowser::Lznt1::Decompress(src, tiny),
                    std::runtime_error);
  }
}
