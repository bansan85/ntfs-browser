#include <ntfs-browser/win-types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <ntfs-browser/bench/bench.h>
#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/disk-readers/memory-disk-reader.h>
#include <ntfs-browser/test/fake-image/fake-ntfs-image.h>
#include <ntfs-browser/tool/device-access/sparse-capture.h>

#include "bench-session-guard.h"
#include "io/file-reader.h"
#include "temp-dir.h"

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/stat.h>
#endif

namespace Bench = NTFS_BROWSER_NAMESPACE::Bench;
namespace Cache = NTFS_BROWSER_NAMESPACE::Cache;
namespace Io = NTFS_BROWSER_NAMESPACE::Io;
namespace Tests = NTFS_BROWSER_NAMESPACE::Tests;
using Catch::Matchers::ContainsSubstring;
using NtfsDevice::SparseCapture;

namespace {

// The unit of the zero test of SparseCapture, a sector of 4096 bytes.
constexpr size_t piece = 4096;

std::vector<BYTE> Data(size_t size, BYTE value) {
  return std::vector<BYTE>(size, value);
}

std::vector<BYTE> Zeros(size_t size) {
  return std::vector<BYTE>(size, BYTE{0});
}

std::vector<BYTE> Join(std::initializer_list<std::vector<BYTE>> parts) {
  std::vector<BYTE> joined;
  for (const std::vector<BYTE>& part : parts) {
    joined.insert(joined.end(), part.begin(), part.end());
  }
  return joined;
}

std::vector<BYTE> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Bytes the file system really allocated to the file: less than its size when
// it holds holes.
std::uint64_t AllocatedBytes(const std::filesystem::path& path) {
#ifdef _WIN32
  DWORD high = 0;
  const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
  return (static_cast<std::uint64_t>(high) << 32U) | low;
#else
  struct stat info{};
  REQUIRE(stat(path.c_str(), &info) == 0);
  return static_cast<std::uint64_t>(info.st_blocks) * 512;
#endif
}

}  // namespace

TEST_CASE("A capture writes the data of a read at its offset",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  const std::filesystem::path path = dir.Path() / "capture.img";
  const std::vector<BYTE> bytes = Data(2 * piece, BYTE{0x5A});
  {
    SparseCapture capture(path);
    REQUIRE(capture.Ok());
    capture.Write(3 * piece, bytes);
    CHECK(capture.Ok());
    CHECK(capture.Error().empty());
  }

  const std::vector<BYTE> file = ReadFile(path);
  REQUIRE(file.size() == 5 * piece);
  CHECK(file == Join({Zeros(3 * piece), bytes}));
}

TEST_CASE("A capture skips the pieces that are all zeros",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  const std::filesystem::path path = dir.Path() / "capture.img";
  {
    SparseCapture capture(path);
    REQUIRE(capture.Ok());
    // Zeros at both ends: nothing is written there, so the file ends with the
    // last piece that holds data.
    capture.Write(0, Join({Zeros(piece), Data(piece, BYTE{1}), Zeros(piece)}));
  }

  CHECK(std::filesystem::file_size(path) == 2 * piece);
}

TEST_CASE("A capture of zeros only leaves the file empty",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  const std::filesystem::path path = dir.Path() / "capture.img";
  {
    SparseCapture capture(path);
    REQUIRE(capture.Ok());
    capture.Write(0, Zeros(8 * piece));
    capture.Write(1 << 20U, Zeros(piece));
    CHECK(capture.Ok());
  }

  CHECK(std::filesystem::file_size(path) == 0);
}

TEST_CASE("A capture writes the data on both sides of a hole",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  const std::filesystem::path path = dir.Path() / "capture.img";
  const std::vector<BYTE> read = Join(
      {Data(piece, BYTE{2}), Zeros(4 * piece), Data(piece + 100, BYTE{3})});
  {
    SparseCapture capture(path);
    REQUIRE(capture.Ok());
    capture.Write(piece, read);
  }

  const std::vector<BYTE> file = ReadFile(path);
  CHECK(file == Join({Zeros(piece), read}));
}

TEST_CASE("A zero piece after a piece with data is not written",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  const std::filesystem::path path = dir.Path() / "capture.img";
  {
    SparseCapture capture(path);
    REQUIRE(capture.Ok());
    // The first piece holds one non-zero byte: it is written whole, and the
    // zero piece after it is not.
    std::vector<BYTE> read = Zeros(piece + 1000);
    read.at(10) = 9;
    capture.Write(0, read);
  }

  CHECK(std::filesystem::file_size(path) == piece);
}

TEST_CASE("A capture of a large hole allocates no space for it",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  const std::filesystem::path path = dir.Path() / "capture.img";
  constexpr size_t hole = 16 * 1024 * 1024;
  {
    SparseCapture capture(path);
    REQUIRE(capture.Ok());
    capture.Write(0, Data(piece, BYTE{1}));
    capture.Write(hole, Data(piece, BYTE{2}));
  }

  CHECK(std::filesystem::file_size(path) == hole + piece);
  if (AllocatedBytes(path) >= hole) {
    SKIP("the temp folder is on a file system without sparse files");
  }
  CHECK(AllocatedBytes(path) < hole / 4);
}

TEST_CASE("SetSize gives the file the size of the volume, and never shrinks it",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  const std::filesystem::path path = dir.Path() / "capture.img";
  constexpr std::uint64_t volume = 1U << 20U;
  {
    SparseCapture capture(path);
    REQUIRE(capture.Ok());
    capture.Write(0, Data(piece, BYTE{1}));
    CHECK(capture.SetSize(volume));
    CHECK(capture.SetSize(10));
    CHECK(capture.Ok());
  }

  CHECK(std::filesystem::file_size(path) == volume);
  const std::vector<BYTE> file = ReadFile(path);
  REQUIRE(file.size() == volume);
  CHECK(file.at(0) == 1);
  CHECK(file.at(piece - 1) == 1);
  CHECK(file.at(piece) == 0);
  CHECK(file.at(volume - 1) == 0);
}

TEST_CASE("Bytes written past the size given stay",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  const std::filesystem::path path = dir.Path() / "capture.img";
  constexpr std::uint64_t volume = 1U << 16U;
  {
    SparseCapture capture(path);
    REQUIRE(capture.Ok());
    capture.Write(volume + piece, Data(piece, BYTE{4}));
    CHECK(capture.SetSize(volume));
  }

  CHECK(std::filesystem::file_size(path) == volume + (2 * piece));
}

TEST_CASE("A capture that cannot be created says why and ignores writes",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  SparseCapture capture(dir.Path() / "no-folder" / "capture.img");

  CHECK_FALSE(capture.Ok());
  CHECK_THAT(capture.Error(), ContainsSubstring("cannot create"));
  capture.Write(0, Data(piece, BYTE{1}));
  CHECK_FALSE(capture.Ok());
  CHECK_FALSE(capture.SetSize(100));
}

TEST_CASE("A capture sink set in the library receives every read",
          "[device-access][capture]") {
  const Tests::TempDir dir;
  const std::filesystem::path path = dir.Path() / "capture.img";
  const std::vector<BYTE> image = Tests::BuildFakeNtfsImage();
  std::vector<BYTE> boot(piece);
  {
    auto capture = std::make_shared<SparseCapture>(path);
    REQUIRE(capture->Ok());
    const Tests::CaptureSinkGuard sink(capture);
    // A capture runs without a session.
    CHECK_FALSE(Bench::Enabled());
    const Io::FileReader<Cache::Strategy::NoCache> file(
        std::make_unique<Tests::MemoryDiskReader>(image));
    LARGE_INTEGER addr{.QuadPart = 0};
    REQUIRE(file.ReadInto(addr, boot));
  }

  const std::vector<BYTE> file = ReadFile(path);
  REQUIRE(file.size() <= image.size());
  REQUIRE_FALSE(file.empty());
  // Zero pieces are holes, and a piece that was written is what the read gave.
  for (size_t index = 0; index < file.size(); index++) {
    if (file.at(index) != 0) {
      CHECK(file.at(index) == boot.at(index));
    }
  }
}
