#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include "memory-disk-reader.h"
#include "sequential-disk-reader.h"

using NtfsBrowserTests::MakeFileStreamProducer;
using NtfsBrowserTests::MakeGeneratorProducer;
using NtfsBrowserTests::MakeMemoryProducer;
using NtfsBrowserTests::MemoryDiskReader;
using NtfsBrowserTests::SequentialDiskReader;

namespace
{

// Size of the buffer the MemoryDiskReader tests read from, where they read,
// and how much.
constexpr size_t kMemoryContentSize = 256;
constexpr size_t kMemoryReadOffset = 100;
constexpr size_t kMemoryReadSize = 32;

// A buffer too small for the read asked of it.
constexpr size_t kTinyContentSize = 16;

// The file loaded by Open(): its size, and the window read back from it.
constexpr size_t kFileContentSize = 64;
constexpr size_t kFileReadSize = 16;
constexpr LONGLONG kFileReadOffset = 10;

// The sequential tests: a source of three chunks, the chunk size, and an
// address the reader must ignore.
constexpr size_t kSequentialContentSize = 48;
constexpr size_t kChunkSize = 16;
constexpr LONGLONG kIgnoredAddress = 12345;

// A source smaller than one chunk, and the size of a generated block.
constexpr size_t kShortSourceSize = 8;
constexpr size_t kGeneratedBlockSize = 8;

// Builds size bytes of distinct, predictable content for read checks.
std::vector<BYTE> MakeContent(size_t size)
{
  std::vector<BYTE> content(size);
  for (size_t i = 0; i < content.size(); i++)
  {
    // i < content.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    content[i] = gsl::narrow<BYTE>(i);
  }
  return content;
}

}  // namespace

TEST_CASE("MemoryDiskReader reads from a buffer given at construction",
          "[disk-reader][memory]")
{
  const std::vector<BYTE> content = MakeContent(kMemoryContentSize);
  MemoryDiskReader reader(content);

  std::array<BYTE, kMemoryReadSize> dest{};
  LARGE_INTEGER addr{.QuadPart = kMemoryReadOffset};
  REQUIRE(reader.ReadInto(addr, dest));

  for (size_t i = 0; i < dest.size(); i++)
  {
    // i < dest.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(dest[i] == content.at(kMemoryReadOffset + i));
  }
}

TEST_CASE("MemoryDiskReader fails reads past the end of its buffer",
          "[disk-reader][memory]")
{
  MemoryDiskReader reader(MakeContent(kTinyContentSize));

  std::array<BYTE, kMemoryReadSize> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("MemoryDiskReader::Open loads a file's content into memory",
          "[disk-reader][memory]")
{
  const std::vector<BYTE> content = MakeContent(kFileContentSize);

  std::random_device rd;
  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     (L"ntfsbrowser-memory-disk-reader-test-" +
                                      std::to_wstring(rd()) + L".bin");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()),
              gsl::narrow<std::streamsize>(content.size()));
  }

  MemoryDiskReader reader(std::vector<BYTE>{});
  REQUIRE(reader.Open(path.wstring()));
  std::filesystem::remove(path);

  std::array<BYTE, kFileReadSize> dest{};
  LARGE_INTEGER addr{.QuadPart = kFileReadOffset};
  REQUIRE(reader.ReadInto(addr, dest));
  for (size_t i = 0; i < dest.size(); i++)
  {
    // i < dest.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(dest[i] == content.at(kFileReadOffset + i));
  }
}

TEST_CASE("SequentialDiskReader ignores addr and reads memory data in order",
          "[disk-reader][sequential]")
{
  const std::vector<BYTE> content = MakeContent(kSequentialContentSize);
  SequentialDiskReader reader(MakeMemoryProducer(content));

  std::array<BYTE, kChunkSize> first{};
  std::array<BYTE, kChunkSize> second{};
  LARGE_INTEGER addr{.QuadPart = kIgnoredAddress};

  REQUIRE(reader.ReadInto(addr, first));
  REQUIRE(reader.ReadInto(addr, second));

  for (size_t i = 0; i < first.size(); i++)
  {
    // i < first.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(first[i] == content.at(i));
  }
  for (size_t i = 0; i < second.size(); i++)
  {
    // i < second.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(second[i] == content.at(kChunkSize + i));
  }
}

TEST_CASE("SequentialDiskReader fails once its memory source is exhausted",
          "[disk-reader][sequential]")
{
  SequentialDiskReader reader(
      MakeMemoryProducer(MakeContent(kShortSourceSize)));

  std::array<BYTE, kChunkSize> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("SequentialDiskReader streams a file source incrementally",
          "[disk-reader][sequential]")
{
  const std::vector<BYTE> content = MakeContent(kSequentialContentSize);

  std::random_device rd;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-sequential-disk-reader-test-" + std::to_wstring(rd()) +
       L".bin");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()),
              gsl::narrow<std::streamsize>(content.size()));
  }

  std::array<BYTE, kChunkSize> first{};
  std::array<BYTE, kChunkSize> second{};
  // Closes reader (and its ifstream) before the file is removed below.
  {
    SequentialDiskReader reader(MakeFileStreamProducer(path));

    LARGE_INTEGER addr{.QuadPart = 0};
    REQUIRE(reader.ReadInto(addr, first));
    REQUIRE(reader.ReadInto(addr, second));
  }

  std::filesystem::remove(path);

  for (size_t i = 0; i < first.size(); i++)
  {
    // i < first.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(first[i] == content.at(i));
  }
  for (size_t i = 0; i < second.size(); i++)
  {
    // i < second.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(second[i] == content.at(kChunkSize + i));
  }
}

TEST_CASE("SequentialDiskReader generates data lazily with no backing store",
          "[disk-reader][sequential]")
{
  size_t calls = 0;
  SequentialDiskReader reader(MakeGeneratorProducer(
      [&calls](std::span<BYTE> dest)
      {
        std::ranges::fill(dest, gsl::narrow<BYTE>(calls));
        calls++;
      }));

  std::array<BYTE, kGeneratedBlockSize> first{};
  std::array<BYTE, kGeneratedBlockSize> second{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(reader.ReadInto(addr, first));
  REQUIRE(reader.ReadInto(addr, second));

  CHECK(calls == 2);
  // first and second are arrays of kGeneratedBlockSize = 8 bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(first[0] == 0);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(second[0] == 1);
}
