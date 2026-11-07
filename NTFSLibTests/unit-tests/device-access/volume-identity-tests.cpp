#include <filesystem>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/test/device-access/temp-dir.h>
#include <ntfs-browser/tool/device-access/volume-identity.h>

namespace ToolDeviceAccess = NTFS_BROWSER_NAMESPACE::Tool::DeviceAccess;

using NTFS_BROWSER_NAMESPACE::Test::DeviceAccess::TempDir;
using ToolDeviceAccess::HostVolumeId;

TEST_CASE("HostVolumeId gives the same answer inside one folder",
          "[device-access][volume-identity]") {
  const TempDir directory;
  const std::filesystem::path file = directory.Write("a.txt", "x");

  const std::optional<std::string> folder_id = HostVolumeId(directory.Path());
  const std::optional<std::string> file_id = HostVolumeId(file);

  REQUIRE(folder_id.has_value());
  CHECK_FALSE(folder_id->empty());
  CHECK(file_id == folder_id);
}

TEST_CASE("HostVolumeId takes the answer of the nearest folder that exists",
          "[device-access][volume-identity]") {
  const TempDir directory;

  const std::optional<std::string> missing =
      HostVolumeId(directory.Path() / "not" / "there" / "yet");

  REQUIRE(missing.has_value());
  CHECK(missing == HostVolumeId(directory.Path()));
}

TEST_CASE("HostVolumeId makes a relative path absolute",
          "[device-access][volume-identity]") {
  const std::optional<std::string> relative = HostVolumeId("no-such-folder");

  REQUIRE(relative.has_value());
  CHECK(relative == HostVolumeId(std::filesystem::current_path()));
}
