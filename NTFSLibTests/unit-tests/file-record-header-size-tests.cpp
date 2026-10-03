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

#include <ntfs-browser/strategy.h>

#include "catch2/matchers/catch_matchers.hpp"
#include "data/file-record-header.h"
#include "file-record-header-edit.h"

using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::FileRecordHeaderImpl;
using NtfsBrowser::kFileRecordMagic;
using NtfsBrowser::kUpdateSequenceStride;
using NtfsBrowser::Strategy;

namespace
{

// Builds a well-formed record header of exactly bufferSize bytes, with
// offset_of_attr set to whatever the caller passes in.
std::vector<BYTE> MakeWellFormedBuffer(size_t bufferSize, WORD offsetOfAttr)
{
  std::vector<BYTE> storage(bufferSize, 0);

  const size_t sectors = bufferSize / kUpdateSequenceStride;
  const WORD offsetOfUs = gsl::narrow<WORD>(bufferSize - 2 * (1 + sectors));

  NtfsBrowserTests::EditFileRecordHeader(storage,
                                         [&](FileRecordHeader::Data& header)
                                         {
                                           header.magic = kFileRecordMagic;
                                           header.offset_of_us = offsetOfUs;
                                           header.size_of_us =
                                               gsl::narrow<WORD>(1 + sectors);
                                           header.offset_of_attr = offsetOfAttr;
                                         });

  return storage;
}

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileRecordHeader must accept a well-formed 4096-byte buffer "
    "(4Kn volumes)",
    "[file-record-header][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  constexpr size_t kBufferSize = 4096;

  const std::vector<BYTE> storage = MakeWellFormedBuffer(kBufferSize, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  // FULL_CACHE's ctor memcpy()s the whole buffer into a fixed-size Data
  // member; a too-small member here would overflow it.
  const auto header = FileRecordHeaderImpl<S>(buffer);
  CHECK(header.GetData()->magic == kFileRecordMagic);
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecordHeader must reject a buffer larger than kMaxFileRecordSize "
    "with a clear, specific message",
    "[file-record-header][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  constexpr size_t kTooBig = 8192;

  const std::vector<BYTE> storage = MakeWellFormedBuffer(kTooBig, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  CHECK_THROWS_MATCHES(
      (FileRecordHeaderImpl<S>(buffer)), std::runtime_error,
      Catch::Matchers::MessageMatches(
          Catch::Matchers::ContainsSubstring("exceeds the maximum")));
}

TEMPLATE_TEST_CASE_SIG(
    "FileRecordHeader::HeaderCommon must bound offset_of_attr against this "
    "instance's own buffer size, not raw[]'s static capacity",
    "[file-record-header][regression]", ((Strategy S), S), Strategy::NO_CACHE,
    Strategy::FULL_CACHE)
{
  constexpr size_t kDeclaredBufferSize = 2048;
  // Past this instance's buffer, but within raw[]'s static capacity.
  constexpr WORD kOffsetPastOwnSize = 3000;

  const std::vector<BYTE> storage =
      MakeWellFormedBuffer(kDeclaredBufferSize, kOffsetPastOwnSize);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  auto header = FileRecordHeaderImpl<S>(buffer);

  // A larger offset_of_attr would build a pointer past the real,
  // 2048-byte allocation backing NO_CACHE's span.
  CHECK(header.HeaderCommon() == nullptr);
}
