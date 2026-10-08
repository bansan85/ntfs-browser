#include <ntfs-browser/win-types.h>

#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/strategy.h>

#include "data/file-record-header.h"
#include "file-record-header-edit.h"

using NtfsBrowser::file_record_magic;
using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::FileRecordHeaderImpl;
using NtfsBrowser::Strategy;

namespace {

// Matches the record buffer size FileRecord always allocates.
constexpr size_t declared_buffer_size = 1024;

// Number of WORDs FileRecordHeader reads into the US array: one per
// 512-byte block.
constexpr size_t array_words =
    declared_buffer_size / NtfsBrowser::update_sequence_stride;

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
    "FileRecordHeader must not leak bytes past the declared buffer when "
    "offset_of_us leaves no room for the US array",
    "[file-record-header][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  // Bytes past declared_buffer_size are outside what FileRecordHeader sees.
  std::vector<BYTE> storage(declared_buffer_size + array_words * sizeof(WORD),
                            0);

  NtfsBrowserTests::EditFileRecordHeader(
      storage, [](FileRecordHeader::Data& header) {
        header.magic = file_record_magic;
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
    const auto header = FileRecordHeaderImpl<S>(buffer);

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
    "FileRecordHeader::PatchUS must restore the last word of every 512-byte "
    "block, whatever the volume's sector size (4Kn volumes)",
    "[file-record-header][regression]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  constexpr size_t record_size = 4096;
  constexpr size_t block_size = 512;
  constexpr size_t blocks = record_size / block_size;
  constexpr WORD usn = 0x7777;
  constexpr WORD offset_of_us_array = 48;

  std::vector<BYTE> storage(record_size, 0);
  NtfsBrowserTests::EditFileRecordHeader(
      storage, [](FileRecordHeader::Data& header) {
        header.magic = file_record_magic;
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
  auto header = FileRecordHeaderImpl<S>(buffer);

  REQUIRE(header.PatchUS());

  for (size_t i = 0; i < blocks; i++) {
    WORD restored = 0;
    std::memcpy(&restored,
                &header.GetData()->raw[(i + 1) * block_size - sizeof(WORD)],
                sizeof(restored));
    CHECK(restored == gsl::narrow<WORD>(us_array_fill_base + i));
  }
}
