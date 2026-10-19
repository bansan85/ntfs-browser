#include <filesystem>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "device-path.h"
#include "temp-dir.h"
#include "text-convert.h"

using NTFS_BROWSER_NAMESPACE::Tests::TempDir;
using NtfsDevice::SplitDeviceTarget;
using NtfsDevice::WideFromPath;

namespace {

// The wide form of the target made of base and the parts after it, joined
// with '/', which both platforms accept as a separator.
std::wstring Target(const std::filesystem::path& base,
                    const std::wstring& rest) {
  return WideFromPath(base) + L"/" + rest;
}

}  // namespace

TEST_CASE("SplitDeviceTarget names an image file with no directory",
          "[device-access][device-path]") {
  const TempDir dir;
  const std::filesystem::path image = dir.Write("disk.img", "x");

  const auto target = SplitDeviceTarget(WideFromPath(image));

  REQUIRE(target.has_value());
  CHECK(target->device == image);
  CHECK(target->directory.empty());
}

TEST_CASE("SplitDeviceTarget splits the directory off an image file",
          "[device-access][device-path]") {
  const TempDir dir;
  const std::filesystem::path image = dir.Write("disk.img", "x");

  const auto target = SplitDeviceTarget(Target(image, L"Windows/System32"));

  REQUIRE(target.has_value());
  CHECK(target->device == image);
  CHECK(target->directory == L"Windows\\System32");
}

TEST_CASE("SplitDeviceTarget drops trailing separators",
          "[device-access][device-path]") {
  const TempDir dir;
  const std::filesystem::path image = dir.Write("disk.img", "x");

  const auto target = SplitDeviceTarget(Target(image, L"Windows//"));

  REQUIRE(target.has_value());
  CHECK(target->device == image);
  CHECK(target->directory == L"Windows");
}

TEST_CASE("SplitDeviceTarget takes the longest leading file",
          "[device-access][device-path]") {
  const TempDir dir;
  const std::filesystem::path outer = dir.Write("outer.img", "x");
  std::filesystem::create_directories(dir.Path() / "sub");
  const std::filesystem::path inner = dir.Write("sub/inner.img", "x");

  const auto target = SplitDeviceTarget(WideFromPath(inner) + L"/Docs");

  REQUIRE(target.has_value());
  CHECK(target->device == inner);
  CHECK(target->directory == L"Docs");
}

TEST_CASE("SplitDeviceTarget refuses a target with no file in it",
          "[device-access][device-path]") {
  const TempDir dir;

  CHECK_FALSE(
      SplitDeviceTarget(Target(dir.Path(), L"missing/Windows")).has_value());
  CHECK_FALSE(SplitDeviceTarget(WideFromPath(dir.Path())).has_value());
  CHECK_FALSE(SplitDeviceTarget(L"").has_value());
  CHECK_FALSE(SplitDeviceTarget(L"windows-no-such-file").has_value());
}

#ifndef _WIN32

TEST_CASE("SplitDeviceTarget keeps a backslash in a POSIX file name",
          "[device-access][device-path]") {
  const TempDir dir;
  const std::filesystem::path image = dir.Write("a\\b.img", "x");

  const auto target = SplitDeviceTarget(WideFromPath(image));

  REQUIRE(target.has_value());
  CHECK(target->device == image);
  CHECK(target->directory.empty());
}

#endif  // !_WIN32
