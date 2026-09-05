#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/file-record-header.h>
#include <ntfs-browser/strategy.h>

using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::kFileRecordMagic;
using NtfsBrowser::Strategy;

namespace
{

// Builds a well-formed record header of exactly bufferSize bytes, with
// offset_of_attr set to whatever the caller passes in.
std::vector<BYTE> MakeWellFormedBuffer(size_t bufferSize, size_t sectorSize,
                                       WORD offsetOfAttr)
{
  std::vector<BYTE> storage(bufferSize, 0);

  const size_t sectors = bufferSize / sectorSize;
  const WORD offsetOfUs = static_cast<WORD>(bufferSize - 2 * (1 + sectors));

  auto& header = *reinterpret_cast<FileRecordHeader::Data*>(storage.data());
  header.magic = kFileRecordMagic;
  header.offset_of_us = offsetOfUs;
  header.size_of_us = static_cast<WORD>(1 + sectors);
  header.offset_of_attr = offsetOfAttr;

  return storage;
}

}  // namespace

TEST_CASE(
    "FileRecordHeader must accept a well-formed 4096-byte buffer under "
    "NO_CACHE (4Kn volumes)",
    "[file-record-header][regression]")
{
  constexpr size_t kBufferSize = 4096;
  constexpr size_t kSectorSize = 4096;

  const std::vector<BYTE> storage =
      MakeWellFormedBuffer(kBufferSize, kSectorSize, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  const auto fr =
      FileRecordHeader::Factory<Strategy::NO_CACHE>(buffer, kSectorSize);
  CHECK(fr.GetData()->magic == kFileRecordMagic);
}

TEST_CASE(
    "FileRecordHeader must accept a well-formed 4096-byte buffer under "
    "FULL_CACHE (4Kn volumes)",
    "[file-record-header][regression]")
{
  constexpr size_t kBufferSize = 4096;
  constexpr size_t kSectorSize = 4096;

  const std::vector<BYTE> storage =
      MakeWellFormedBuffer(kBufferSize, kSectorSize, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  // FULL_CACHE's ctor memcpy()s the whole buffer into a fixed-size Data
  // member; a too-small member here would overflow it.
  const auto fr =
      FileRecordHeader::Factory<Strategy::FULL_CACHE>(buffer, kSectorSize);
  CHECK(fr.GetData()->magic == kFileRecordMagic);
}

TEST_CASE(
    "FileRecordHeader must reject a buffer larger than kMaxFileRecordSize "
    "with a clear, specific message",
    "[file-record-header][regression]")
{
  constexpr size_t kTooBig = 8192;
  constexpr size_t kSectorSize = 4096;

  const std::vector<BYTE> storage =
      MakeWellFormedBuffer(kTooBig, kSectorSize, 64);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  CHECK_THROWS_MATCHES(
      (FileRecordHeader::Factory<Strategy::NO_CACHE>(buffer, kSectorSize)),
      std::runtime_error,
      Catch::Matchers::MessageMatches(
          Catch::Matchers::ContainsSubstring("exceeds the maximum")));
}

TEST_CASE(
    "FileRecordHeader::HeaderCommon must bound offset_of_attr against this "
    "instance's own buffer size, not raw[]'s static capacity",
    "[file-record-header][regression]")
{
  constexpr size_t kDeclaredBufferSize = 2048;
  constexpr size_t kSectorSize = 2048;
  // Past this instance's buffer, but within raw[]'s static capacity.
  constexpr WORD kOffsetPastOwnSize = 3000;

  const std::vector<BYTE> storage = MakeWellFormedBuffer(
      kDeclaredBufferSize, kSectorSize, kOffsetPastOwnSize);
  const std::span<const BYTE> buffer(storage.data(), storage.size());

  auto fr = FileRecordHeader::Factory<Strategy::NO_CACHE>(buffer, kSectorSize);

  // A larger offset_of_attr would build a pointer past the real,
  // 2048-byte allocation backing NO_CACHE's span.
  CHECK(fr.HeaderCommon() == nullptr);
}
