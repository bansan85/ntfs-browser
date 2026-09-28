#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/strategy.h>

#include "file-reader.h"
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
