#include <ntfs-browser/win-types.h>

#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <gsl/narrow>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "data/file-record-header.h"
#include "file-record-header-edit.h"
#include "record/header.h"

namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
using NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader;
using NTFS_BROWSER_NAMESPACE::Record::HeaderImpl;

namespace {

// Matches the record buffer size FileRecord always allocates.
constexpr size_t declared_buffer_size = 1024;

// Number of WORDs Record::Header reads into the US array: one per
// 512-byte block.
constexpr size_t array_words =
    declared_buffer_size /
    NTFS_BROWSER_NAMESPACE::Data::FileRecordHeader::update_sequence_stride;

// Places the US array's first word exactly at the buffer's declared end.
constexpr WORD offset_of_us_value =
    static_cast<WORD>(declared_buffer_size - sizeof(WORD));

// Bases of the two WORD patterns below.
constexpr WORD sentinel_base = 0xBEEF;
constexpr WORD us_array_fill_base = 0xA000;

// Pattern that never appears anywhere in the declared 1024-byte buffer.
WORD Sentinel(size_t index) { return gsl::narrow<WORD>(sentinel_base + index); }

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "Record::Header must not leak bytes past the declared buffer when "
    "offset_of_us leaves no room for the US array",
    "[file-record-header][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  // Bytes past declared_buffer_size are outside what Record::Header sees.
  std::vector<BYTE> storage(declared_buffer_size + array_words * sizeof(WORD),
                            0);

  NTFS_BROWSER_NAMESPACE::Tests::EditFileRecordHeader(
      storage, [](FileRecordHeader& header) {
        header.magic = FileRecordHeader::file_record_magic;
        header.offset_of_us = offset_of_us_value;
        // Correct value; it bounds how many array words the ctor reads.
        header.size_of_us = static_cast<WORD>(array_words + 1);
      });

  for (size_t i = 0; i < array_words; i++) {
    const WORD sentinel = Sentinel(i);
    std::memcpy(&storage.at(declared_buffer_size + (i * sizeof(WORD))),
                &sentinel, sizeof(sentinel));
  }

  const std::span<const BYTE> buffer(storage.data(), declared_buffer_size);

  bool leaked_sentinel = false;
  try {
    const auto header = HeaderImpl<S>(buffer);

    leaked_sentinel = header.us_array.size() == array_words && [&] {
      for (size_t i = 0; i < array_words; i++) {
        // us_array.size() == array_words was tested first.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        if (header.us_array[i] != Sentinel(i)) {
          return false;
        }
      }
      return true;
    }();
  } catch (const std::runtime_error&) {
    // A fix may reject the record outright instead of truncating it.
    leaked_sentinel = false;
  }

  CHECK_FALSE(leaked_sentinel);
}

TEMPLATE_TEST_CASE_SIG(
    "Record::Header::PatchUS must restore the last word of every 512-byte "
    "block, whatever the volume's sector size (4Kn volumes)",
    "[file-record-header][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  constexpr size_t record_size = 4096;
  constexpr size_t block_size = 512;
  constexpr size_t blocks = record_size / block_size;
  constexpr WORD usn = 0x7777;
  constexpr WORD offset_of_us_array = 48;

  std::vector<BYTE> storage(record_size, 0);
  NTFS_BROWSER_NAMESPACE::Tests::EditFileRecordHeader(
      storage, [](FileRecordHeader& header) {
        header.magic = FileRecordHeader::file_record_magic;
        header.offset_of_us = offset_of_us_array;
        header.size_of_us = static_cast<WORD>(blocks + 1);
      });

  const auto put_word = [&](size_t offset, WORD value) {
    std::memcpy(&storage.at(offset), &value, sizeof(value));
  };

  put_word(offset_of_us_array, usn);
  for (size_t i = 0; i < blocks; i++) {
    // The array holds each block's true last word; the block itself carries
    // the sequence number, as it does on disk.
    put_word(offset_of_us_array + sizeof(WORD) * (1 + i),
             gsl::narrow<WORD>(us_array_fill_base + i));
    put_word((i + 1) * block_size - sizeof(WORD), usn);
  }

  const std::span<const BYTE> buffer(storage.data(), storage.size());
  auto header = HeaderImpl<S>(buffer);

  REQUIRE(header.PatchUS());

  for (size_t i = 0; i < blocks; i++) {
    WORD restored = 0;
    std::memcpy(&restored,
                &header.GetData()->raw[(i + 1) * block_size - sizeof(WORD)],
                sizeof(restored));
    CHECK(restored == gsl::narrow<WORD>(us_array_fill_base + i));
  }
}

namespace {

// Builds a well-formed record header of exactly bufferSize bytes, with
// offset_of_attr set to whatever the caller passes in.
std::vector<BYTE> MakeWellFormedBuffer(size_t buffer_size,
                                       WORD offset_of_attr) {
  std::vector<BYTE> storage(buffer_size, 0);

  const size_t sectors = buffer_size / FileRecordHeader::update_sequence_stride;
  const WORD offset_of_us = gsl::narrow<WORD>(buffer_size - 2 * (1 + sectors));

  NTFS_BROWSER_NAMESPACE::Tests::EditFileRecordHeader(
      storage, [&](FileRecordHeader& header) {
        header.magic = FileRecordHeader::file_record_magic;
        header.offset_of_us = offset_of_us;
        header.size_of_us = gsl::narrow<WORD>(1 + sectors);
        header.offset_of_attr = offset_of_attr;
      });

  return storage;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "Record::Header must accept a well-formed 4096-byte buffer "
    "(4Kn volumes)",
    "[file-record-header][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  constexpr size_t buffer_size_value = 4096;

  const std::vector<BYTE> storage = MakeWellFormedBuffer(buffer_size_value, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  // FullCache's ctor memcpy()s the whole buffer into a fixed-size Data
  // member; a too-small member here would overflow it.
  const auto header = HeaderImpl<S>(buffer);
  CHECK(header.GetData()->magic == FileRecordHeader::file_record_magic);
}

TEMPLATE_TEST_CASE_SIG(
    "Record::Header must reject a buffer larger than max_file_record_size "
    "with a clear, specific message",
    "[file-record-header][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  constexpr size_t too_big = 8192;

  const std::vector<BYTE> storage = MakeWellFormedBuffer(too_big, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  CHECK_THROWS_MATCHES(
      (HeaderImpl<S>(buffer)), std::runtime_error,
      Catch::Matchers::MessageMatches(
          Catch::Matchers::ContainsSubstring("exceeds the maximum")));
}

TEMPLATE_TEST_CASE_SIG(
    "Record::Header::HeaderCommon must bound offset_of_attr against this "
    "instance's own buffer size, not raw[]'s static capacity",
    "[file-record-header][regression]", ((Cache::Strategy S), S),
    Cache::Strategy::NoCache, Cache::Strategy::FullCache) {
  constexpr size_t declared_buffer_size = 2048;
  // Past this instance's buffer, but within raw[]'s static capacity.
  constexpr WORD offset_past_own_size = 3000;

  const std::vector<BYTE> storage =
      MakeWellFormedBuffer(declared_buffer_size, offset_past_own_size);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  const auto header = HeaderImpl<S>(buffer);

  // A larger offset_of_attr would build a pointer past the real,
  // 2048-byte allocation backing NoCache's span.
  CHECK(header.HeaderCommon() == nullptr);
}
