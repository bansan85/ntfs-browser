#include <memory>
#include <optional>
#include <vector>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/cache/strategy.h>
#include <ntfs-browser/namespace.h>
#include <ntfs-browser/ntfs-volume.h>
#include <ntfs-browser/volume-options.h>

#include "fake-ntfs-image.h"
#include "memory-disk-reader.h"
#include "volume-open.h"

using NTFS_BROWSER_NAMESPACE::NtfsVolume;
using NTFS_BROWSER_NAMESPACE::Cache::Strategy;
using NTFS_BROWSER_NAMESPACE::Tests::BuildFakeNtfsImageWithMftTree;
using NTFS_BROWSER_NAMESPACE::Tests::MemoryDiskReader;
using NtfsCompare::comparison_volume_options;

TEST_CASE("The comparison volume options are the library defaults",
          "[ntfscompare][volume-open]") {
  CHECK_FALSE(comparison_volume_options.include_deleted);
  CHECK_FALSE(comparison_volume_options.recover_errors);
}

TEMPLATE_TEST_CASE_SIG(
    "A volume built with the comparison options reports them back",
    "[ntfscompare][volume-open]", ((Strategy S), S), Strategy::NoCache,
    Strategy::FullCache) {
  const NtfsVolume<S> volume(
      std::make_unique<MemoryDiskReader>(BuildFakeNtfsImageWithMftTree()),
      comparison_volume_options);
  REQUIRE(volume.IsVolumeOK());

  CHECK_FALSE(volume.GetOptions().include_deleted);
  CHECK_FALSE(volume.GetOptions().recover_errors);
}

#ifndef _WIN32

  #include <filesystem>
  #include <string>

  #include "device-path.h"
  #include "temp-dir.h"
  #include "text-convert.h"

using NTFS_BROWSER_NAMESPACE::Tests::TempDir;
using NTFS_BROWSER_NAMESPACE::Tests::WriteFakeNtfsImage;
using NtfsCompare::FindMountOfDevice;
using NtfsCompare::OpenDeviceVolume;

TEST_CASE("An image file is a device target",
          "[ntfscompare][volume-open][device]") {
  const TempDir dir;
  const std::filesystem::path image = dir.Write("disk.img", "x");

  const auto target = NtfsDevice::SplitDeviceTarget(
      NtfsDevice::WideFromPath(image) + L"/Windows");

  REQUIRE(target.has_value());
  CHECK(target->device == image);
  CHECK(target->directory == L"Windows");
}

TEST_CASE("A device that does not exist is not a device target",
          "[ntfscompare][volume-open][device]") {
  CHECK_FALSE(
      NtfsDevice::SplitDeviceTarget(L"/dev/ntfscompare-no-such-device/Windows")
          .has_value());
}

TEST_CASE("OpenDeviceVolume opens both strategies over an image file",
          "[ntfscompare][volume-open][device]") {
  const std::filesystem::path image =
      WriteFakeNtfsImage(BuildFakeNtfsImageWithMftTree());
  const auto handles =
      OpenDeviceVolume({.device = image, .directory = L"Docs\\Sub"});
  std::filesystem::remove(image);

  REQUIRE(handles.has_value());
  CHECK(handles->full_cache->IsVolumeOK());
  CHECK(handles->no_cache->IsVolumeOK());
  CHECK(handles->relative_path == L"Docs/Sub");
  CHECK_FALSE(handles->mount_point.has_value());
  CHECK(handles->mount_fstype.empty());
}

TEST_CASE("OpenDeviceVolume refuses a file that is not an NTFS volume",
          "[ntfscompare][volume-open][device]") {
  const TempDir dir;
  const std::filesystem::path file =
      dir.Write("not-ntfs.img", std::string(4096, '\0'));

  CHECK_FALSE(OpenDeviceVolume({.device = file, .directory = L""}).has_value());
}

TEST_CASE("OpenDeviceVolume refuses a device it cannot open",
          "[ntfscompare][volume-open][device]") {
  const TempDir dir;

  CHECK_FALSE(
      OpenDeviceVolume({.device = dir.Path() / "missing.img", .directory = L""})
          .has_value());
}

TEST_CASE("FindMountOfDevice returns the mount point and type of a device",
          "[ntfscompare][volume-open][device]") {
  const TempDir dir;
  const std::filesystem::path device = dir.Write("disk.img", "x");
  const std::filesystem::path table = dir.Write(
      "mounts", "/dev/other /mnt/other ext4 rw 0 0\n" + device.string() +
                    " /mnt/win fuse rw,nosuid 0 0\n");

  const auto mount = FindMountOfDevice(device, table.c_str());

  REQUIRE(mount.has_value());
  CHECK(mount->mount_point == "/mnt/win");
  CHECK(mount->fstype == "fuse");
}

TEST_CASE("FindMountOfDevice follows a symbolic link to the device",
          "[ntfscompare][volume-open][device]") {
  const TempDir dir;
  const std::filesystem::path device = dir.Write("disk.img", "x");
  const std::filesystem::path link = dir.Path() / "by-uuid";
  std::filesystem::create_symlink(device, link);
  const std::filesystem::path table =
      dir.Write("mounts", link.string() + " /mnt/win ntfs3 ro 0 0\n");

  const auto mount = FindMountOfDevice(device, table.c_str());

  REQUIRE(mount.has_value());
  CHECK(mount->mount_point == "/mnt/win");
  CHECK(mount->fstype == "ntfs3");
}

TEST_CASE("FindMountOfDevice finds nothing for an unmounted device",
          "[ntfscompare][volume-open][device]") {
  const TempDir dir;
  const std::filesystem::path device = dir.Write("disk.img", "x");
  const std::filesystem::path table =
      dir.Write("mounts", "/dev/other /mnt/other ext4 rw 0 0\n");

  CHECK_FALSE(FindMountOfDevice(device, table.c_str()).has_value());
  CHECK_FALSE(FindMountOfDevice(device, (dir.Path() / "no-mounts").c_str())
                  .has_value());
}

TEST_CASE("FindMountOfDevice takes the last mount of a device",
          "[ntfscompare][volume-open][device]") {
  const TempDir dir;
  const std::filesystem::path device = dir.Write("disk.img", "x");
  const std::filesystem::path table =
      dir.Write("mounts", device.string() + " /mnt/first fuse rw 0 0\n" +
                              device.string() + " /mnt/second fuse rw 0 0\n");

  const auto mount = FindMountOfDevice(device, table.c_str());

  REQUIRE(mount.has_value());
  CHECK(mount->mount_point == "/mnt/second");
}

#endif  // !_WIN32
