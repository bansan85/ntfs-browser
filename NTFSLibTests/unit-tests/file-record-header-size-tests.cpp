#include <ntfs-browser/win-types.h>

#include <cstddef>
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

#include "catch2/matchers/catch_matchers.hpp"
#include "data/file-record-header.h"
#include "file-record-header-edit.h"
#include "record/header.h"

namespace Cache = NtfsBrowser::Cache;
using NtfsBrowser::Data::FileRecordHeader;
using NtfsBrowser::Record::HeaderImpl;

namespace {

// Builds a well-formed record header of exactly bufferSize bytes, with
// offset_of_attr set to whatever the caller passes in.
std::vector<BYTE> MakeWellFormedBuffer(size_t buffer_size,
                                       WORD offset_of_attr) {
  std::vector<BYTE> storage(buffer_size, 0);

  const size_t sectors = buffer_size / FileRecordHeader::update_sequence_stride;
  const WORD offset_of_us = gsl::narrow<WORD>(buffer_size - 2 * (1 + sectors));

  NtfsBrowserTests::EditFileRecordHeader(
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
