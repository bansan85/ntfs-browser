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
constexpr size_t memory_content_size = 256;
constexpr size_t memory_read_offset = 100;
constexpr size_t memory_read_size = 32;

// A buffer too small for the read asked of it.
constexpr size_t tiny_content_size = 16;

// The file loaded by Open(): its size, and the window read back from it.
constexpr size_t file_content_size = 64;
constexpr size_t file_read_size = 16;
constexpr LONGLONG file_read_offset = 10;

// The sequential tests: a source of three chunks, the chunk size, and an
// address the reader must ignore.
constexpr size_t sequential_content_size = 48;
constexpr size_t chunk_size = 16;
constexpr LONGLONG ignored_address = 12345;

// A source smaller than one chunk, and the size of a generated block.
constexpr size_t short_source_size = 8;
constexpr size_t generated_block_size = 8;

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
  const std::vector<BYTE> content = MakeContent(memory_content_size);
  MemoryDiskReader reader(content);

  std::array<BYTE, memory_read_size> dest{};
  LARGE_INTEGER addr{.QuadPart = memory_read_offset};
  REQUIRE(reader.ReadInto(addr, dest));

  for (size_t i = 0; i < dest.size(); i++)
  {
    // i < dest.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(dest[i] == content.at(memory_read_offset + i));
  }
}

TEST_CASE("MemoryDiskReader fails reads past the end of its buffer",
          "[disk-reader][memory]")
{
  MemoryDiskReader reader(MakeContent(tiny_content_size));

  std::array<BYTE, memory_read_size> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("MemoryDiskReader::Open loads a file's content into memory",
          "[disk-reader][memory]")
{
  const std::vector<BYTE> content = MakeContent(file_content_size);

  std::random_device random_device;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-memory-disk-reader-test-" +
       std::to_wstring(random_device()) + L".bin");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()),
              gsl::narrow<std::streamsize>(content.size()));
  }

  MemoryDiskReader reader(std::vector<BYTE>{});
  REQUIRE(reader.Open(path.wstring()));
  std::filesystem::remove(path);

  std::array<BYTE, file_read_size> dest{};
  LARGE_INTEGER addr{.QuadPart = file_read_offset};
  REQUIRE(reader.ReadInto(addr, dest));
  for (size_t i = 0; i < dest.size(); i++)
  {
    // i < dest.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(dest[i] == content.at(file_read_offset + i));
  }
}

TEST_CASE("SequentialDiskReader ignores addr and reads memory data in order",
          "[disk-reader][sequential]")
{
  const std::vector<BYTE> content = MakeContent(sequential_content_size);
  SequentialDiskReader reader(MakeMemoryProducer(content));

  std::array<BYTE, chunk_size> first{};
  std::array<BYTE, chunk_size> second{};
  LARGE_INTEGER addr{.QuadPart = ignored_address};

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
    CHECK(second[i] == content.at(chunk_size + i));
  }
}

TEST_CASE("SequentialDiskReader fails once its memory source is exhausted",
          "[disk-reader][sequential]")
{
  SequentialDiskReader reader(
      MakeMemoryProducer(MakeContent(short_source_size)));

  std::array<BYTE, chunk_size> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("SequentialDiskReader streams a file source incrementally",
          "[disk-reader][sequential]")
{
  const std::vector<BYTE> content = MakeContent(sequential_content_size);

  std::random_device random_device;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-sequential-disk-reader-test-" +
       std::to_wstring(random_device()) + L".bin");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()),
              gsl::narrow<std::streamsize>(content.size()));
  }

  std::array<BYTE, chunk_size> first{};
  std::array<BYTE, chunk_size> second{};
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
    CHECK(second[i] == content.at(chunk_size + i));
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

  std::array<BYTE, generated_block_size> first{};
  std::array<BYTE, generated_block_size> second{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(reader.ReadInto(addr, first));
  REQUIRE(reader.ReadInto(addr, second));

  CHECK(calls == 2);
  // first and second are arrays of generated_block_size = 8 bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(first[0] == 0);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(second[0] == 1);
}
