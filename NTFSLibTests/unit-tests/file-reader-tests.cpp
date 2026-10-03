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

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/strategy.h>

#include "file-reader.h"
#include "memory-disk-reader.h"
#include "partition-disk-reader.h"

namespace
{

// Size of the backing file the read tests fill with a byte pattern.
constexpr size_t kContentSize = 4096;

// Multiplier of the byte pattern, so a shifted read cannot match by accident.
constexpr size_t kPatternStep = 7;

// Where, and how much, the ReadInto test reads.
constexpr size_t kReadIntoOffset = 1000;
constexpr size_t kReadIntoSize = 128;

// A file too small for the read asked of it, and its fill byte.
constexpr size_t kTinyFileSize = 16;
constexpr BYTE kTinyFileFill = 0xAB;

// The two reads of the buffer-growth test: a small one, then a larger one at
// another offset.
constexpr DWORD kFirstReadSize = 16;
constexpr LONGLONG kSecondReadOffset = 100;
constexpr DWORD kSecondReadSize = 2048;

// Opens path through a PartitionDiskReader (offset 0) instead of
// FileReader's own Open(), which only exists on Windows (Win32DiskReader).
template <NtfsBrowser::Strategy S>
NtfsBrowser::FileReader<S> OpenOnDisk(const std::filesystem::path& path)
{
  auto reader = std::make_unique<NtfsBrowserTests::PartitionDiskReader>(0);
  REQUIRE(reader->Open(path.wstring()));
  return NtfsBrowser::FileReader<S>(std::move(reader));
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
            gsl::narrow<std::streamsize>(content.size()));

  return path;
}

struct TempFile final
{
  std::filesystem::path path;

  explicit TempFile(std::span<const BYTE> content)
      : path(WriteTempFile(content))
  {
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  TempFile(TempFile&&) = delete;
  TempFile& operator=(TempFile&&) = delete;
  ~TempFile() { std::filesystem::remove(path); }
};

}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "FileReader::ReadInto reads into the caller-provided buffer",
    "[file-reader]", ((NtfsBrowser::Strategy S), S),
    NtfsBrowser::Strategy::NO_CACHE, NtfsBrowser::Strategy::FULL_CACHE)
{
  std::vector<BYTE> content(kContentSize);
  for (size_t i = 0; i < content.size(); i++)
  {
    // i < content.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    content[i] = static_cast<BYTE>(i);
  }
  TempFile const file(content);

  NtfsBrowser::FileReader<S> const reader = OpenOnDisk<S>(file.path);

  std::array<BYTE, kReadIntoSize> dest{};
  LARGE_INTEGER addr{.QuadPart = kReadIntoOffset};
  REQUIRE(reader.ReadInto(addr, dest));

  for (size_t i = 0; i < dest.size(); i++)
  {
    // i < dest.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(dest[i] == content.at(kReadIntoOffset + i));
  }
}

TEMPLATE_TEST_CASE_SIG("FileReader::ReadInto fails past end of file",
                       "[file-reader]", ((NtfsBrowser::Strategy S), S),
                       NtfsBrowser::Strategy::NO_CACHE,
                       NtfsBrowser::Strategy::FULL_CACHE)
{
  std::vector<BYTE> content(kTinyFileSize, kTinyFileFill);
  TempFile const file(content);

  NtfsBrowser::FileReader<S> const reader = OpenOnDisk<S>(file.path);

  std::array<BYTE, kReadIntoSize> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("FileReader NO_CACHE Read grows its buffer before filling it",
          "[file-reader]")
{
  std::vector<BYTE> content(kContentSize);
  for (size_t i = 0; i < content.size(); i++)
  {
    // i < content.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    content[i] = static_cast<BYTE>(i * kPatternStep);
  }

  NtfsBrowser::FileReader<NtfsBrowser::Strategy::NO_CACHE> const reader(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(content));

  LARGE_INTEGER first_addr{.QuadPart = 0};
  const auto first = reader.Read(first_addr, kFirstReadSize);
  REQUIRE(first.has_value());
  REQUIRE(first->size() == kFirstReadSize);

  LARGE_INTEGER second_addr{.QuadPart = kSecondReadOffset};
  const auto second = reader.Read(second_addr, kSecondReadSize);
  REQUIRE(second.has_value());
  REQUIRE(second->size() == kSecondReadSize);
  for (size_t i = 0; i < second->size(); i++)
  {
    // i < second->size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    REQUIRE((*second)[i] == content.at(kSecondReadOffset + i));
  }
}

#ifdef _WIN32
TEMPLATE_TEST_CASE_SIG(
    "FileReader::Open honours the length of a non NUL-terminated view",
    "[file-reader]", ((NtfsBrowser::Strategy S), S),
    NtfsBrowser::Strategy::NO_CACHE, NtfsBrowser::Strategy::FULL_CACHE)
{
  const std::vector<BYTE> content(64, 0x5A);
  TempFile file(content);

  const std::wstring realPath = file.path.wstring();
  const std::wstring longer = realPath + L"xyz";

  NtfsBrowser::FileReader<S> reader;
  REQUIRE(reader.Open(std::wstring_view(longer.data(), realPath.size())));

  std::array<BYTE, kTinyFileSize> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(reader.ReadInto(addr, dest));
  // dest holds kTinyFileSize = 16 bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(dest[0] == 0x5A);
}
#endif
