#include <ntfs-browser/win-types.h>

#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "data/file-record-header.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "record/header.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Data = NTFS_BROWSER_NAMESPACE::Data;

namespace {

// Offset of the update sequence array in the fixup-only buffers, which carry no
// attributes.
constexpr WORD fixup_us_offset = 48;

// Byte offset of the first byte of the fake record that the update sequence
// array's second word holds.
constexpr size_t second_sector_end = 1022;
// Byte offset of the first sector's last word.
constexpr size_t first_sector_end = 510;

// Record buffer whose update sequence array sits at us_offset, with the given
// sector-end words.
std::vector<BYTE> FixupBuffer(WORD first_end, WORD second_end) {
  std::vector<BYTE> buffer(NTFS_BROWSER_NAMESPACE::Tests::fake_file_record_size,
                           0);
  NTFS_BROWSER_NAMESPACE::Tests::EditFileRecordHeader(
      buffer, [](Data::FileRecordHeader& header) {
        header.magic = Data::FileRecordHeader::file_record_magic;
        header.offset_of_us = fixup_us_offset;
        header.size_of_us = 3;
      });

  constexpr WORD usn = 1;
  constexpr WORD first_original = 5;
  constexpr WORD second_original = 6;
  const auto put = [&buffer](size_t offset, WORD value) {
    std::memcpy(&buffer.at(offset), &value, sizeof(value));
  };
  put(fixup_us_offset, usn);
  put(fixup_us_offset + 2, first_original);
  put(fixup_us_offset + 4, second_original);
  put(first_sector_end, first_end);
  put(second_sector_end, second_end);
  return buffer;
}

// Builds a Record::Header over a buffer too small for a file record header.
void ConstructHeaderOver(std::span<const BYTE> buffer) {
  const NTFS_BROWSER_NAMESPACE::Record::HeaderImpl<Cache::Strategy::NoCache>
      header(buffer);
  static_cast<void>(header.buffer_size);
}

}  // namespace

TEST_CASE("a file record buffer shorter than its header is refused",
          "[cov-attrs]") {
  const std::vector<BYTE> tiny(16, 0);
  REQUIRE_THROWS_AS(ConstructHeaderOver(tiny), std::runtime_error);
}

TEST_CASE(
    "a sector end that does not carry the update sequence fails the fixup",
    "[cov-attrs]") {
  // Second sector end matches; the first carries neither the USN nor its
  // original.
  auto buffer = FixupBuffer(2, 1);
  NTFS_BROWSER_NAMESPACE::Record::HeaderImpl<Cache::Strategy::NoCache> header(
      buffer);
  CHECK_FALSE(header.PatchUS());
}

TEST_CASE("a sector end that carries the update sequence is restored",
          "[cov-attrs]") {
  auto buffer = FixupBuffer(1, 1);
  NTFS_BROWSER_NAMESPACE::Record::HeaderImpl<Cache::Strategy::NoCache> header(
      buffer);
  CHECK(header.PatchUS());
  CHECK(buffer[first_sector_end] == 5);
  CHECK(buffer[second_sector_end] == 6);
}
