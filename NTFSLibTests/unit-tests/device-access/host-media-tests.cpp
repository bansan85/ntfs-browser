#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>
#ifdef __linux__
  #include <sys/sysmacros.h>
#endif

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/host-media.h>
#include <ntfs-browser/tool/device-access/posix-device-disk-reader.h>
#include <ntfs-browser/tool/device-access/text-convert.h>
#include <ntfs-browser/tuning/tuning.h>

#include "temp-dir.h"

namespace fs = std::filesystem;
namespace Tuning = NTFS_BROWSER_NAMESPACE::Tuning;
using NTFS_BROWSER_NAMESPACE::Tests::TempDir;
using NtfsDevice::PosixDeviceDiskReader;
using NtfsDevice::RotationalOfBlockDevice;
using NtfsDevice::SysfsRoot;
using NtfsDevice::WideFromPath;

namespace {

// Writes content to path, and makes the folders above it.
void WriteFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << content;
}

// A sysfs tree under root with one disk, devices/disk/<disk>/, that says
// rotational (0 or 1) and holds the partition <disk>1;
// dev/block/<major>:<minor> links to that partition, as sysfs does.
void MakeDiskWithPartition(const fs::path& root, const std::string& disk,
                           unsigned major, unsigned minor,
                           const std::string& rotational) {
  const fs::path disk_node = root / "devices" / "disk" / disk;
  WriteFile(disk_node / "queue" / "rotational", rotational + "\n");
  WriteFile(disk_node / (disk + "1") / "partition", "1\n");
  fs::create_directories(root / "dev" / "block");
  fs::create_directory_symlink(
      disk_node / (disk + "1"),
      root / "dev" / "block" /
          (std::to_string(major) + ":" + std::to_string(minor)));
}

// The device number of the file system that holds path.
struct DeviceNumber {
  unsigned major{0};
  unsigned minor{0};
};

DeviceNumber DeviceOf(const fs::path& path) {
  struct stat info{};
  REQUIRE(stat(path.c_str(), &info) == 0);
#ifdef __linux__
  return {major(info.st_dev), minor(info.st_dev)};
#else
  return {};
#endif
}

// The variable that makes the reader ask no sysfs. The tests set it before
// main(); a test of the sysfs walk clears it.
constexpr const char* skip_variable = "NTFS_BROWSER_NO_HOST_MEDIA_QUERY";

// Points SysfsRoot() at root until the end of the scope, then back where the
// tests keep it. The skip variable is cleared for the scope.
class SysfsRootScope final {
 public:
  explicit SysfsRootScope(const fs::path& root) : previous_(SysfsRoot()) {
    NtfsDevice::SetSysfsRootForTests(root);
    unsetenv(skip_variable);
  }

  SysfsRootScope(const SysfsRootScope&) = delete;
  SysfsRootScope& operator=(const SysfsRootScope&) = delete;
  SysfsRootScope(SysfsRootScope&&) = delete;
  SysfsRootScope& operator=(SysfsRootScope&&) = delete;

  ~SysfsRootScope() {
    NtfsDevice::SetSysfsRootForTests(previous_);
    setenv(skip_variable, "1", 1);
  }

 private:
  fs::path previous_;
};

}  // namespace

TEST_CASE("RotationalOfBlockDevice reads the disk behind a partition",
          "[device-access][host-media]") {
  const TempDir dir;
  MakeDiskWithPartition(dir.Path(), "sda", 8, 1, "1");
  MakeDiskWithPartition(dir.Path(), "sdb", 8, 17, "0");

  CHECK(RotationalOfBlockDevice(8, 1, dir.Path()) == true);
  CHECK(RotationalOfBlockDevice(8, 17, dir.Path()) == false);
}

TEST_CASE("RotationalOfBlockDevice reads a device that is a disk itself",
          "[device-access][host-media]") {
  const TempDir dir;
  const fs::path disk = dir.Path() / "devices" / "disk" / "nvme0n1";
  WriteFile(disk / "queue" / "rotational", "0\n");
  fs::create_directories(dir.Path() / "dev" / "block");
  fs::create_directory_symlink(disk, dir.Path() / "dev" / "block" / "259:0");

  CHECK(RotationalOfBlockDevice(259, 0, dir.Path()) == false);
}

TEST_CASE("RotationalOfBlockDevice says nothing when sysfs does not",
          "[device-access][host-media]") {
  const TempDir dir;

  SECTION("no tree at all") {
    CHECK_FALSE(RotationalOfBlockDevice(8, 1, dir.Path()).has_value());
    CHECK_FALSE(
        RotationalOfBlockDevice(8, 1, dir.Path() / "missing").has_value());
  }

  SECTION("a device number that no node names") {
    MakeDiskWithPartition(dir.Path(), "sda", 8, 1, "1");
    CHECK_FALSE(RotationalOfBlockDevice(8, 2, dir.Path()).has_value());
  }

  SECTION("a disk with no queue/rotational") {
    const fs::path disk = dir.Path() / "devices" / "disk" / "loop0";
    fs::create_directories(disk / "loop0p1");
    WriteFile(disk / "loop0p1" / "partition", "1\n");
    fs::create_directories(dir.Path() / "dev" / "block");
    fs::create_directory_symlink(disk / "loop0p1",
                                 dir.Path() / "dev" / "block" / "7:1");
    CHECK_FALSE(RotationalOfBlockDevice(7, 1, dir.Path()).has_value());
  }
}

#ifdef __linux__

TEST_CASE("PosixDeviceDiskReader over an image file takes the kind of its disk",
          "[device-access][host-media][posix-reader]") {
  const TempDir image_dir;
  const fs::path image = image_dir.Write("disk.img", std::string(4096, 'x'));
  const DeviceNumber device = DeviceOf(image);
  const TempDir sysfs;

  SECTION("a rotational disk") {
    MakeDiskWithPartition(sysfs.Path(), "sda", device.major, device.minor, "1");
    const SysfsRootScope scope(sysfs.Path());
    PosixDeviceDiskReader reader;
    REQUIRE(reader.Open(WideFromPath(image)));
    CHECK(reader.GetMediaKind() == Tuning::MediaKind::Rotational);
  }

  SECTION("a solid state disk") {
    MakeDiskWithPartition(sysfs.Path(), "sda", device.major, device.minor, "0");
    const SysfsRootScope scope(sysfs.Path());
    PosixDeviceDiskReader reader;
    REQUIRE(reader.Open(WideFromPath(image)));
    CHECK(reader.GetMediaKind() == Tuning::MediaKind::SolidState);
  }

  SECTION("the skip variable keeps the reader from asking sysfs") {
    MakeDiskWithPartition(sysfs.Path(), "sda", device.major, device.minor, "1");
    const SysfsRootScope scope(sysfs.Path());
    setenv(skip_variable, "1", 1);
    PosixDeviceDiskReader reader;
    REQUIRE(reader.Open(WideFromPath(image)));
    CHECK(reader.GetMediaKind() == Tuning::MediaKind::Unknown);
  }

  SECTION("no tree: Unknown") {
    const SysfsRootScope scope(sysfs.Path());
    PosixDeviceDiskReader reader;
    REQUIRE(reader.Open(WideFromPath(image)));
    CHECK(reader.GetMediaKind() == Tuning::MediaKind::Unknown);
  }
}

TEST_CASE("The tests keep the sysfs tree on a folder that holds nothing",
          "[device-access][host-media][posix-reader]") {
  const TempDir image_dir;
  const fs::path image = image_dir.Write("disk.img", std::string(4096, 'x'));

  PosixDeviceDiskReader reader;
  REQUIRE(reader.Open(WideFromPath(image)));

  CHECK_FALSE(fs::exists(SysfsRoot()));
  CHECK(reader.GetMediaKind() == Tuning::MediaKind::Unknown);
}

#endif  // __linux__
