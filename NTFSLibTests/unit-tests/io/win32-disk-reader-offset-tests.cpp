#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <gsl/narrow>
#include <windows.h>
#include <winioctl.h>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

#include "io/file-reader.h"

namespace {

using Reader = NTFS_BROWSER_NAMESPACE::Io::FileReader<
    NTFS_BROWSER_NAMESPACE::Cache::Strategy::NoCache>;

// Size of the backing file of the ordinary read tests.
constexpr size_t content_size = 8192;

// Multiplier and modulus of the byte pattern. The modulus is prime and not a
// power of two, so a read shifted by any power of two cannot match by accident.
constexpr uint64_t pattern_step = 7;
constexpr uint64_t pattern_modulus = 251;

// How much each read asks for.
constexpr size_t chunk_size = 64;

// A stream of reads at scattered offsets: how many, and the fixed seed.
constexpr size_t scattered_reads = 200;
constexpr unsigned scatter_seed = 12345;

// The sparse file tests. The marker straddles the 4 GiB boundary, where
// OffsetHigh changes. The second one lies well above it.
constexpr LONGLONG four_gib = LONGLONG{1} << 32;
constexpr LONGLONG straddling_marker_offset = four_gib - 16;
constexpr LONGLONG high_marker_offset = (LONGLONG{5} << 30) + 100;
constexpr LONGLONG hole_offset = four_gib + (LONGLONG{1} << 29);

// An offset far past the end of any file these tests create.
constexpr LONGLONG far_past_end = LONGLONG{1} << 40;

// The byte the pattern holds at a file offset.
BYTE PatternAt(uint64_t offset) {
  return gsl::narrow<BYTE>((offset * pattern_step) % pattern_modulus);
}

// chunk_size bytes of the pattern, starting at offset.
std::array<BYTE, chunk_size> PatternChunk(uint64_t offset) {
  std::array<BYTE, chunk_size> chunk{};
  for (size_t i = 0; i < chunk.size(); i++) {
    // i < chunk.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    chunk[i] = PatternAt(offset + i);
  }
  return chunk;
}

// A temporary file that is removed when it goes out of scope.
class TempFile {
 public:
  TempFile() {
    std::random_device random_device;
    path_ = std::filesystem::temp_directory_path() /
            (L"ntfsbrowser-win32-disk-reader-test-" +
             std::to_wstring(random_device()) + L".bin");
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

  // Fills the file with size bytes of the pattern.
  void WritePattern(size_t size) const {
    std::vector<char> bytes(size);
    for (size_t i = 0; i < bytes.size(); i++) {
      // i < bytes.size() by the loop condition.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
      bytes[i] = gsl::narrow_cast<char>(PatternAt(i));
    }
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), gsl::narrow<std::streamsize>(bytes.size()));
  }

 private:
  std::filesystem::path path_;
};

// Opens path through the production Win32DiskReader.
Reader OpenReader(const TempFile& file) {
  Reader reader;
  REQUIRE(reader.Open(file.Path().wstring()));
  return reader;
}

// Reads chunk_size bytes at offset. True when the reader reported success.
bool ReadChunk(const Reader& reader, LONGLONG offset,
               std::array<BYTE, chunk_size>& dest) {
  LARGE_INTEGER addr{.QuadPart = offset};
  return reader.ReadInto(addr, dest);
}

using HandlePtr =
    std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&::CloseHandle)>;

// Writes bytes at offset of an open file, through the handle's file pointer.
void WriteAt(HANDLE handle, LONGLONG offset, std::span<const BYTE> bytes) {
  LARGE_INTEGER position{.QuadPart = offset};
  REQUIRE(SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != FALSE);
  DWORD written = 0;
  REQUIRE(WriteFile(handle, bytes.data(), gsl::narrow<DWORD>(bytes.size()),
                    &written, nullptr) != FALSE);
  REQUIRE(written == bytes.size());
}

// Creates a sparse file with a pattern chunk at each offset in markers. False
// when the volume has no sparse file support: filling 5 GiB is not an option.
bool MakeSparseFile(const TempFile& file, std::span<const LONGLONG> markers) {
  HandlePtr handle(CreateFileW(file.Path().c_str(),
                               GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr),
                   &CloseHandle);
  REQUIRE(handle.get() != INVALID_HANDLE_VALUE);

  DWORD returned = 0;
  if (DeviceIoControl(handle.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0,
                      &returned, nullptr) == FALSE) {
    return false;
  }

  for (const LONGLONG offset : markers) {
    const std::array<BYTE, chunk_size> chunk =
        PatternChunk(gsl::narrow<uint64_t>(offset));
    WriteAt(handle.get(), offset, chunk);
  }
  return true;
}

}  // namespace

TEST_CASE("Win32DiskReader reads a plain file at unordered offsets",
          "[disk-reader][win32]") {
  const TempFile file;
  file.WritePattern(content_size);
  const Reader reader = OpenReader(file);

  std::mt19937 generator(scatter_seed);
  std::uniform_int_distribution<LONGLONG> offsets(
      0, gsl::narrow<LONGLONG>(content_size - chunk_size));

  for (size_t i = 0; i < scattered_reads; i++) {
    const LONGLONG offset = offsets(generator);
    std::array<BYTE, chunk_size> dest{};
    REQUIRE(ReadChunk(reader, offset, dest));
    CHECK(dest == PatternChunk(gsl::narrow<uint64_t>(offset)));
  }
}

TEST_CASE("Win32DiskReader reads backwards, then at the start and the end",
          "[disk-reader][win32]") {
  const TempFile file;
  file.WritePattern(content_size);
  const Reader reader = OpenReader(file);

  const std::array<LONGLONG, 6> offsets{
      gsl::narrow<LONGLONG>(content_size - chunk_size), 4096, 1000, 1, 0,
      gsl::narrow<LONGLONG>(content_size - chunk_size)};
  for (const LONGLONG offset : offsets) {
    std::array<BYTE, chunk_size> dest{};
    REQUIRE(ReadChunk(reader, offset, dest));
    CHECK(dest == PatternChunk(gsl::narrow<uint64_t>(offset)));
  }
}

TEST_CASE("Win32DiskReader gives the same bytes on repeated reads",
          "[disk-reader][win32]") {
  const TempFile file;
  file.WritePattern(content_size);
  const Reader reader = OpenReader(file);

  constexpr LONGLONG repeated_offset = 3000;
  constexpr LONGLONG other_offset = 100;
  const std::array<BYTE, chunk_size> expected = PatternChunk(repeated_offset);

  for (size_t i = 0; i < 3; i++) {
    std::array<BYTE, chunk_size> dest{};
    REQUIRE(ReadChunk(reader, repeated_offset, dest));
    CHECK(dest == expected);

    // Another read between two identical ones moves nothing the next relies on.
    std::array<BYTE, chunk_size> other{};
    REQUIRE(ReadChunk(reader, other_offset, other));
    CHECK(other == PatternChunk(other_offset));
  }
}

TEST_CASE("Win32DiskReader fails a read that runs past the end of the file",
          "[disk-reader][win32]") {
  const TempFile file;
  file.WritePattern(content_size);
  const Reader reader = OpenReader(file);

  std::array<BYTE, chunk_size> dest{};

  SECTION("a read ending exactly at the end succeeds") {
    CHECK(ReadChunk(reader, gsl::narrow<LONGLONG>(content_size - chunk_size),
                    dest));
  }

  SECTION("a read one byte too long fails") {
    CHECK_FALSE(ReadChunk(
        reader, gsl::narrow<LONGLONG>(content_size - chunk_size + 1), dest));
  }

  SECTION("a read starting at the end fails") {
    CHECK_FALSE(ReadChunk(reader, gsl::narrow<LONGLONG>(content_size), dest));
  }

  SECTION("a read far past the end fails") {
    CHECK_FALSE(ReadChunk(reader, far_past_end, dest));
  }
}

TEST_CASE("Win32DiskReader fails a negative address", "[disk-reader][win32]") {
  const TempFile file;
  file.WritePattern(content_size);
  const Reader reader = OpenReader(file);

  std::array<BYTE, chunk_size> dest{};
  CHECK_FALSE(ReadChunk(reader, -1, dest));
  // Parentheses keep windows.h's min macro from expanding.
  CHECK_FALSE(ReadChunk(reader, (std::numeric_limits<LONGLONG>::min)(), dest));

  // A failed read leaves the reader usable.
  REQUIRE(ReadChunk(reader, 0, dest));
  CHECK(dest == PatternChunk(0));
}

TEST_CASE("Win32DiskReader reads above 4 GiB", "[disk-reader][win32]") {
  const TempFile file;
  const std::array<LONGLONG, 2> markers{straddling_marker_offset,
                                        high_marker_offset};
  if (!MakeSparseFile(file, markers)) {
    SKIP("The temporary directory's volume has no sparse file support.");
  }
  const Reader reader = OpenReader(file);

  SECTION("a read straddling the 4 GiB boundary") {
    std::array<BYTE, chunk_size> dest{};
    REQUIRE(ReadChunk(reader, straddling_marker_offset, dest));
    CHECK(dest ==
          PatternChunk(gsl::narrow<uint64_t>(straddling_marker_offset)));
  }

  SECTION("a read well above 4 GiB") {
    std::array<BYTE, chunk_size> dest{};
    REQUIRE(ReadChunk(reader, high_marker_offset, dest));
    CHECK(dest == PatternChunk(gsl::narrow<uint64_t>(high_marker_offset)));
  }

  SECTION("a read in the hole reads zeros") {
    std::array<BYTE, chunk_size> dest{};
    dest.fill(0xFF);
    REQUIRE(ReadChunk(reader, hole_offset, dest));
    CHECK(std::ranges::all_of(dest, [](BYTE b) { return b == 0; }));
  }

  SECTION("a read past the end of the sparse file fails") {
    std::array<BYTE, chunk_size> dest{};
    CHECK_FALSE(ReadChunk(reader, far_past_end, dest));
  }
}
