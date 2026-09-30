#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/strategy.h>

#include "file-reader.h"
#include "memory-disk-reader.h"
#include "partition-disk-reader.h"

namespace
{

// Opens path through a PartitionDiskReader (offset 0) instead of
// FileReader's own Open(), which only exists on Windows (Win32DiskReader).
NtfsBrowser::FileReader<NtfsBrowser::Strategy::NO_CACHE>
    OpenOnDisk(const std::filesystem::path& path)
{
  auto reader = std::make_unique<NtfsBrowserTests::PartitionDiskReader>(0);
  REQUIRE(reader->Open(path.wstring()));
  return NtfsBrowser::FileReader<NtfsBrowser::Strategy::NO_CACHE>(
      std::move(reader));
}

// Writes content to a new temp file and returns its path.
std::filesystem::path WriteTempFile(std::span<const BYTE> content)
{
  std::random_device rd;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-reader-test-" + std::to_wstring(rd()) + L".bin");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(content.data()),
            static_cast<std::streamsize>(content.size()));

  return path;
}

struct TempFile
{
  std::filesystem::path path;

  explicit TempFile(std::span<const BYTE> content)
      : path(WriteTempFile(content))
  {
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  ~TempFile() { std::filesystem::remove(path); }
};

}  // namespace

TEST_CASE("FileReader::ReadInto reads into the caller-provided buffer",
          "[file-reader]")
{
  std::vector<BYTE> content(4096);
  for (size_t i = 0; i < content.size(); i++)
  {
    content[i] = static_cast<BYTE>(i);
  }
  TempFile file(content);

  NtfsBrowser::FileReader<NtfsBrowser::Strategy::NO_CACHE> reader =
      OpenOnDisk(file.path);

  std::array<BYTE, 128> dest{};
  LARGE_INTEGER addr{.QuadPart = 1000};
  REQUIRE(reader.ReadInto(addr, dest));

  for (size_t i = 0; i < dest.size(); i++)
  {
    CHECK(dest[i] == content[1000 + i]);
  }
}

TEST_CASE("FileReader::ReadInto fails past end of file", "[file-reader]")
{
  std::vector<BYTE> content(16, 0xAB);
  TempFile file(content);

  NtfsBrowser::FileReader<NtfsBrowser::Strategy::NO_CACHE> reader =
      OpenOnDisk(file.path);

  std::array<BYTE, 128> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("FileReader NO_CACHE Read grows its buffer before filling it",
          "[file-reader]")
{
  std::vector<BYTE> content(4096);
  for (size_t i = 0; i < content.size(); i++)
  {
    content[i] = static_cast<BYTE>(i * 7);
  }

  NtfsBrowser::FileReader<NtfsBrowser::Strategy::NO_CACHE> reader(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(content));

  LARGE_INTEGER first_addr{.QuadPart = 0};
  const auto first = reader.Read(first_addr, 16);
  REQUIRE(first.has_value());
  REQUIRE(first->size() == 16);

  LARGE_INTEGER second_addr{.QuadPart = 100};
  const auto second = reader.Read(second_addr, 2048);
  REQUIRE(second.has_value());
  REQUIRE(second->size() == 2048);
  for (size_t i = 0; i < second->size(); i++)
  {
    REQUIRE((*second)[i] == content[100 + i]);
  }
}

#ifdef _WIN32
TEST_CASE("FileReader::Open honours the length of a non NUL-terminated view",
          "[file-reader]")
{
  const std::vector<BYTE> content(64, 0x5A);
  TempFile file(content);

  const std::wstring realPath = file.path.wstring();
  const std::wstring longer = realPath + L"xyz";

  NtfsBrowser::FileReader<NtfsBrowser::Strategy::NO_CACHE> reader;
  REQUIRE(reader.Open(std::wstring_view(longer.data(), realPath.size())));

  std::array<BYTE, 16> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(reader.ReadInto(addr, dest));
  CHECK(dest[0] == 0x5A);
}
#endif
