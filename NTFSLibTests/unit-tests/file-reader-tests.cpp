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
#include "optional-access.h"
#include "partition-disk-reader.h"

namespace {

// Size of the backing file the read tests fill with a byte pattern.
constexpr size_t content_size = 4096;

// Multiplier of the byte pattern, so a shifted read cannot match by accident.
constexpr size_t pattern_step = 7;

// Where, and how much, the ReadInto test reads.
constexpr size_t read_into_offset = 1000;
constexpr size_t read_into_size = 128;

// A file too small for the read asked of it, and its fill byte.
constexpr size_t tiny_file_size = 16;
constexpr BYTE tiny_file_fill = 0xAB;

// The two reads of the buffer-growth test: a small one, then a larger one at
// another offset.
constexpr DWORD first_read_size = 16;
constexpr LONGLONG second_read_offset = 100;
constexpr DWORD second_read_size = 2048;

// Opens path through a PartitionDiskReader (offset 0) instead of
// FileReader's own Open(), which only exists on Windows (Win32DiskReader).
template <NtfsBrowser::Strategy S>
NtfsBrowser::Io::FileReader<S> OpenOnDisk(const std::filesystem::path& path) {
  auto reader = std::make_unique<NtfsBrowserTests::PartitionDiskReader>(0);
  REQUIRE(reader->Open(path.wstring()));
  return NtfsBrowser::Io::FileReader<S>(std::move(reader));
}

// Writes content to a new temp file and returns its path.
std::filesystem::path WriteTempFile(std::span<const BYTE> content) {
  std::random_device random_device;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-reader-test-" + std::to_wstring(random_device()) +
       L".bin");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(content.data()),
            gsl::narrow<std::streamsize>(content.size()));

  return path;
}

struct TempFile final {
  std::filesystem::path path;

  explicit TempFile(std::span<const BYTE> content)
      : path(WriteTempFile(content)) {}

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
    NtfsBrowser::Strategy::NoCache, NtfsBrowser::Strategy::FullCache) {
  std::vector<BYTE> content(content_size);
  for (size_t i = 0; i < content.size(); i++) {
    // i < content.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    content[i] = static_cast<BYTE>(i);
  }
  const TempFile file(content);

  const NtfsBrowser::Io::FileReader<S> reader = OpenOnDisk<S>(file.path);

  std::array<BYTE, read_into_size> dest{};
  LARGE_INTEGER addr{.QuadPart = read_into_offset};
  REQUIRE(reader.ReadInto(addr, dest));

  for (size_t i = 0; i < dest.size(); i++) {
    // i < dest.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(dest[i] == content.at(read_into_offset + i));
  }
}

TEMPLATE_TEST_CASE_SIG("FileReader::ReadInto fails past end of file",
                       "[file-reader]", ((NtfsBrowser::Strategy S), S),
                       NtfsBrowser::Strategy::NoCache,
                       NtfsBrowser::Strategy::FullCache) {
  std::vector<BYTE> content(tiny_file_size, tiny_file_fill);
  const TempFile file(content);

  const NtfsBrowser::Io::FileReader<S> reader = OpenOnDisk<S>(file.path);

  std::array<BYTE, read_into_size> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE_FALSE(reader.ReadInto(addr, dest));
}

TEST_CASE("FileReader NoCache Read grows its buffer before filling it",
          "[file-reader]") {
  std::vector<BYTE> content(content_size);
  for (size_t i = 0; i < content.size(); i++) {
    // i < content.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    content[i] = static_cast<BYTE>(i * pattern_step);
  }

  const NtfsBrowser::Io::FileReader<NtfsBrowser::Strategy::NoCache> reader(
      std::make_unique<NtfsBrowserTests::MemoryDiskReader>(content));

  LARGE_INTEGER first_addr{.QuadPart = 0};
  const auto first = reader.Read(first_addr, first_read_size);
  REQUIRE(first.has_value());
  REQUIRE(NtfsBrowserTests::Unwrap(first).size() == first_read_size);

  LARGE_INTEGER second_addr{.QuadPart = second_read_offset};
  const auto second = reader.Read(second_addr, second_read_size);
  REQUIRE(second.has_value());
  REQUIRE(NtfsBrowserTests::Unwrap(second).size() == second_read_size);
  for (size_t i = 0; i < NtfsBrowserTests::Unwrap(second).size(); i++) {
    // i < second->size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    REQUIRE(NtfsBrowserTests::Unwrap(second)[i] ==
            content.at(second_read_offset + i));
  }
}

#ifdef _WIN32
TEMPLATE_TEST_CASE_SIG(
    "FileReader::Open honours the length of a non NUL-terminated view",
    "[file-reader]", ((NtfsBrowser::Strategy S), S),
    NtfsBrowser::Strategy::NoCache, NtfsBrowser::Strategy::FullCache) {
  const std::vector<BYTE> content(64, 0x5A);
  TempFile file(content);

  const std::wstring real_path = file.path.wstring();
  const std::wstring longer = real_path + L"xyz";

  NtfsBrowser::Io::FileReader<S> reader;
  REQUIRE(reader.Open(std::wstring_view(longer.data(), real_path.size())));

  std::array<BYTE, tiny_file_size> dest{};
  LARGE_INTEGER addr{.QuadPart = 0};
  REQUIRE(reader.ReadInto(addr, dest));
  // dest holds tiny_file_size = 16 bytes.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
  CHECK(dest[0] == 0x5A);
}
#endif
