#ifndef _WIN32

  #include <cerrno>
  #include <cstddef>
  #include <filesystem>
  #include <string>
  #include <vector>

  #include <catch2/catch_test_macros.hpp>

  #include <ntfs-browser/namespace.h>
  #include <ntfs-browser/test/device-access/temp-dir.h>

  #include "io/posix-disk-reader.h"
  #include "win-types.h"

namespace Io = NTFS_BROWSER_NAMESPACE::Io;
using NTFS_BROWSER_NAMESPACE::Test::DeviceAccess::TempDir;

namespace {

// Bytes of the image file of the tests.
constexpr std::size_t image_bytes = 4096;

// An image file whose byte at offset n is n modulo 251, a prime that keeps the
// pattern from repeating on a page boundary.
std::string Pattern() {
  std::string bytes(image_bytes, '\0');
  for (std::size_t i = 0; i < bytes.size(); i++) {
    bytes.at(i) = static_cast<char>(i % 251);
  }
  return bytes;
}

}  // namespace

TEST_CASE("A PosixDiskReader reads the bytes at an offset", "[io][posix]") {
  const TempDir dir;
  const std::string content = Pattern();
  const std::filesystem::path file = dir.Write("disk.img", content);

  Io::PosixDiskReader reader;
  REQUIRE(reader.Open(file.wstring()));
  CHECK(reader.Error() == 0);
  CHECK(reader.Size() == image_bytes);

  LARGE_INTEGER addr{.QuadPart = 1000};
  std::vector<BYTE> read(16);
  REQUIRE(reader.ReadInto(addr, read));
  for (std::size_t i = 0; i < read.size(); i++) {
    CHECK(read.at(i) == static_cast<BYTE>(content.at(1000 + i)));
  }
}

TEST_CASE("A PosixDiskReader refuses what it cannot read", "[io][posix]") {
  const TempDir dir;
  const std::filesystem::path file = dir.Write("disk.img", Pattern());
  std::vector<BYTE> read(16);

  SECTION("before it is open") {
    const Io::PosixDiskReader reader;
    LARGE_INTEGER addr{.QuadPart = 0};

    CHECK_FALSE(reader.ReadInto(addr, read));
    CHECK_FALSE(reader.Size());
  }

  SECTION("a negative address") {
    Io::PosixDiskReader reader;
    REQUIRE(reader.Open(file.wstring()));
    LARGE_INTEGER addr{.QuadPart = -1};

    CHECK_FALSE(reader.ReadInto(addr, read));
  }

  SECTION("a read past the end") {
    Io::PosixDiskReader reader;
    REQUIRE(reader.Open(file.wstring()));
    LARGE_INTEGER addr{.QuadPart = image_bytes - 8};

    CHECK_FALSE(reader.ReadInto(addr, read));
  }

  SECTION("a path that does not exist") {
    Io::PosixDiskReader reader;

    CHECK_FALSE(reader.Open((dir.Path() / "no-such.img").wstring()));
    CHECK(reader.Error() == ENOENT);
  }
}

TEST_CASE("A PosixDiskReader opened again reads the new file", "[io][posix]") {
  const TempDir dir;
  const std::filesystem::path first = dir.Write("first.img", "first");
  const std::filesystem::path second = dir.Write("second.img", "second one");

  Io::PosixDiskReader reader;
  REQUIRE(reader.Open(first.wstring()));
  CHECK(reader.Size() == 5);
  REQUIRE(reader.Open(second.wstring()));

  CHECK(reader.Size() == 10);
}

TEST_CASE("A PosixDiskReader gives no size for a file that is not a disk",
          "[io][posix]") {
  Io::PosixDiskReader reader;
  REQUIRE(reader.Open(L"/dev/null"));

  CHECK_FALSE(reader.Size());
}

#endif  // !_WIN32
