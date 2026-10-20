#include <ntfs-browser/win-types.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#include <unistd.h>

#include "posix-device-disk-reader.h"
#include "temp-dir.h"
#include "text-convert.h"

using NTFS_BROWSER_NAMESPACE::Tests::TempDir;
using NtfsDevice::PosixDeviceDiskReader;
using NtfsDevice::WideFromPath;

namespace {

// A 4 KiB file whose byte at offset n is n modulo 251, a prime that keeps the
// pattern from repeating on a page boundary.
std::string Pattern() {
  std::string bytes(4096, '\0');
  for (size_t i = 0; i < bytes.size(); i++) {
    bytes[i] = static_cast<char>(i % 251);
  }
  return bytes;
}

}  // namespace

TEST_CASE("PosixDeviceDiskReader reads the bytes at an offset",
          "[device-access][posix-reader]") {
  const TempDir dir;
  const std::string content = Pattern();
  const std::filesystem::path file = dir.Write("disk.img", content);

  PosixDeviceDiskReader reader;
  REQUIRE(reader.Open(WideFromPath(file)));
  CHECK(reader.LastError() == 0);

  LARGE_INTEGER addr{.QuadPart = 1000};
  std::vector<BYTE> read(16);
  REQUIRE(reader.ReadInto(addr, read));
  for (size_t i = 0; i < read.size(); i++) {
    CHECK(read[i] == static_cast<BYTE>(content[1000 + i]));
  }
}

TEST_CASE("PosixDeviceDiskReader fails a read past the end",
          "[device-access][posix-reader]") {
  const TempDir dir;
  const std::filesystem::path file = dir.Write("disk.img", Pattern());

  PosixDeviceDiskReader reader;
  REQUIRE(reader.Open(WideFromPath(file)));

  LARGE_INTEGER addr{.QuadPart = 4090};
  std::vector<BYTE> read(16);
  CHECK_FALSE(reader.ReadInto(addr, read));
}

TEST_CASE("PosixDeviceDiskReader reports a missing path as ENOENT",
          "[device-access][posix-reader]") {
  const TempDir dir;

  PosixDeviceDiskReader reader;
  CHECK_FALSE(reader.Open(WideFromPath(dir.Path() / "no-such.img")));
  CHECK(reader.LastError() == ENOENT);
}

TEST_CASE("PosixDeviceDiskReader reports an unreadable file as EACCES",
          "[device-access][posix-reader]") {
  if (geteuid() == 0) {
    SKIP("root can read any file");
  }
  const TempDir dir;
  const std::filesystem::path file = dir.Write("locked.img", Pattern());
  REQUIRE(chmod(file.c_str(), 0) == 0);

  PosixDeviceDiskReader reader;
  CHECK_FALSE(reader.Open(WideFromPath(file)));
  CHECK(reader.LastError() == EACCES);
}

TEST_CASE("PosixDeviceDiskReader clears the error after a good open",
          "[device-access][posix-reader]") {
  const TempDir dir;
  const std::filesystem::path file = dir.Write("disk.img", Pattern());

  PosixDeviceDiskReader reader;
  REQUIRE_FALSE(reader.Open(WideFromPath(dir.Path() / "no-such.img")));
  REQUIRE(reader.Open(WideFromPath(file)));
  CHECK(reader.LastError() == 0);
}

TEST_CASE("PosixDeviceDiskReader reports the size of an image file",
          "[device-access][posix-reader]") {
  const TempDir dir;
  const std::string content = Pattern();
  const std::filesystem::path file = dir.Write("disk.img", content);
  const std::filesystem::path empty = dir.Write("empty.img", "");

  PosixDeviceDiskReader reader;
  CHECK_FALSE(reader.Size().has_value());
  REQUIRE(reader.Open(WideFromPath(file)));
  CHECK(reader.Size() == content.size());

  REQUIRE(reader.Open(WideFromPath(empty)));
  CHECK(reader.Size() == 0);
}

TEST_CASE("PosixDeviceDiskReader has no size for a file that is not a disk",
          "[device-access][posix-reader]") {
  const TempDir dir;

  PosixDeviceDiskReader directory;
  REQUIRE(directory.Open(WideFromPath(dir.Path())));
  CHECK_FALSE(directory.Size().has_value());

  // A character device is neither an image file nor a block device.
  PosixDeviceDiskReader null_device;
  REQUIRE(null_device.Open(L"/dev/null"));
  CHECK_FALSE(null_device.Size().has_value());
}
