#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <vector>

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include "memory-disk-reader.h"
#include "sequential-disk-reader.h"

using NtfsBrowserTests::MakeFileStreamProducer;
using NtfsBrowserTests::MakeGeneratorProducer;
using NtfsBrowserTests::MakeMemoryProducer;
using NtfsBrowserTests::MemoryDiskReader;
using NtfsBrowserTests::SequentialDiskReader;

namespace
{

std::vector<BYTE> MakeContent(size_t size)
{
  std::vector<BYTE> content(size);
  for (size_t i = 0; i < content.size(); i++)
  {
    content[i] = static_cast<BYTE>(i);
  }
  return content;
}

}  // namespace

TEST_CASE("MemoryDiskReader reads from a buffer given at construction",
          "[disk-reader][memory]")
{
  const std::vector<BYTE> content = MakeContent(256);
  MemoryDiskReader reader(content);

  std::array<BYTE, 32> dest{};
  LARGE_INTEGER addr{.QuadPart = 100};
  REQUIRE(reader.ReadInto(addr, dest));

  for (size_t i = 0; i < dest.size(); i++)
  {
    CHECK(dest[i] == content[100 + i]);
  }
}

TEST_CASE("MemoryDiskReader fails reads past the end of its buffer",
          "[disk-reader][memory]")
{
  MemoryDiskReader reader(MakeContent(16));

  std::array<BYTE, 32> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("MemoryDiskReader::Open loads a file's content into memory",
          "[disk-reader][memory]")
{
  const std::vector<BYTE> content = MakeContent(64);

  std::random_device rd;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-memory-disk-reader-test-" + std::to_wstring(rd()) +
       L".bin");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()),
              static_cast<std::streamsize>(content.size()));
  }

  MemoryDiskReader reader(std::vector<BYTE>{});
  REQUIRE(reader.Open(path.wstring()));
  std::filesystem::remove(path);

  std::array<BYTE, 16> dest{};
  LARGE_INTEGER addr{.QuadPart = 10};
  REQUIRE(reader.ReadInto(addr, dest));
  for (size_t i = 0; i < dest.size(); i++)
  {
    CHECK(dest[i] == content[10 + i]);
  }
}

TEST_CASE("SequentialDiskReader ignores addr and reads memory data in order",
          "[disk-reader][sequential]")
{
  const std::vector<BYTE> content = MakeContent(48);
  SequentialDiskReader reader(MakeMemoryProducer(content));

  std::array<BYTE, 16> first{};
  std::array<BYTE, 16> second{};
  LARGE_INTEGER addr{.QuadPart = 12345};  // deliberately bogus, must be ignored

  REQUIRE(reader.ReadInto(addr, first));
  REQUIRE(reader.ReadInto(addr, second));

  for (size_t i = 0; i < first.size(); i++)
  {
    CHECK(first[i] == content[i]);
  }
  for (size_t i = 0; i < second.size(); i++)
  {
    CHECK(second[i] == content[16 + i]);
  }
}

TEST_CASE("SequentialDiskReader fails once its memory source is exhausted",
          "[disk-reader][sequential]")
{
  SequentialDiskReader reader(MakeMemoryProducer(MakeContent(8)));

  std::array<BYTE, 16> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("SequentialDiskReader streams a file source incrementally",
          "[disk-reader][sequential]")
{
  const std::vector<BYTE> content = MakeContent(48);

  std::random_device rd;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-sequential-disk-reader-test-" + std::to_wstring(rd()) +
       L".bin");
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()),
              static_cast<std::streamsize>(content.size()));
  }

  std::array<BYTE, 16> first{};
  std::array<BYTE, 16> second{};
  {
    SequentialDiskReader reader(MakeFileStreamProducer(path));

    LARGE_INTEGER addr{.QuadPart = 0};
    REQUIRE(reader.ReadInto(addr, first));
    REQUIRE(reader.ReadInto(addr, second));
  }  // reader (and its ifstream) closed before the file is removed below

  std::filesystem::remove(path);

  for (size_t i = 0; i < first.size(); i++)
  {
    CHECK(first[i] == content[i]);
  }
  for (size_t i = 0; i < second.size(); i++)
  {
    CHECK(second[i] == content[16 + i]);
  }
}

TEST_CASE("SequentialDiskReader generates data lazily with no backing store",
          "[disk-reader][sequential]")
{
  size_t calls = 0;
  SequentialDiskReader reader(MakeGeneratorProducer(
      [&calls](std::span<BYTE> dest)
      {
        std::fill(dest.begin(), dest.end(), static_cast<BYTE>(calls));
        calls++;
      }));

  std::array<BYTE, 8> first{};
  std::array<BYTE, 8> second{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(reader.ReadInto(addr, first));
  REQUIRE(reader.ReadInto(addr, second));

  CHECK(calls == 2);
  CHECK(first[0] == 0);
  CHECK(second[0] == 1);
}
