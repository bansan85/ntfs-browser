#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>
#include <ntfs-browser/tool/device-access/device-path.h>
#include <ntfs-browser/tool/device-access/text-convert.h>

#include "long-path-support.h"
#include "temp-dir.h"

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

TEST_CASE("DriveLetterOf reads only a letter, a colon and a separator",
          "[device-access][device-path]") {
  using NtfsDevice::DriveLetterOf;

  CHECK(DriveLetterOf(L"c") == L'c');
  CHECK(DriveLetterOf(L"C:") == L'C');
  CHECK(DriveLetterOf(L"c:\\") == L'c');
  CHECK(DriveLetterOf(L"d:/") == L'd');

  CHECK_FALSE(DriveLetterOf(L"").has_value());
  CHECK_FALSE(DriveLetterOf(L"c:\\x\\img.bin").has_value());
  CHECK_FALSE(DriveLetterOf(L"c:img.bin").has_value());
  CHECK_FALSE(DriveLetterOf(L"c:\\\\").has_value());
  CHECK_FALSE(DriveLetterOf(L"cc").has_value());
  CHECK_FALSE(DriveLetterOf(L"1:").has_value());
  CHECK_FALSE(DriveLetterOf(L":").has_value());
  CHECK_FALSE(DriveLetterOf(L"\\\\.\\C:").has_value());
  CHECK_FALSE(DriveLetterOf(L"/dev/sda1").has_value());
}

TEST_CASE("ClassifyTarget takes a file for a file and a missing path for none",
          "[device-access][device-path]") {
  using NtfsDevice::ClassifyTarget;
  using NtfsDevice::TargetKind;
  const TempDir dir;
  const std::filesystem::path image = dir.Write("disk.img", "x");

  CHECK(ClassifyTarget(WideFromPath(image)) == TargetKind::File);
  CHECK(ClassifyTarget(WideFromPath(dir.Path() / "absent.img")) ==
        TargetKind::Missing);
  CHECK(ClassifyTarget(WideFromPath(dir.Path())) == TargetKind::Missing);
  CHECK(ClassifyTarget(L"") == TargetKind::Missing);
  CHECK(ClassifyTarget(L"ntfs-bench-test-no-such-target") ==
        TargetKind::Missing);
}

TEST_CASE("ClassifyTarget does not take a file under a drive for the drive",
          "[device-access][device-path]") {
  using NtfsDevice::ClassifyTarget;
  using NtfsDevice::TargetKind;
  const TempDir dir;
  const std::filesystem::path image = dir.Write("disk.img", "x");
  const std::filesystem::path absent = dir.Path() / "absent.img";

  // On Windows the temp folder is under a drive letter, and so is the path.
  CHECK(ClassifyTarget(WideFromPath(image)) != TargetKind::Drive);
  CHECK(ClassifyTarget(WideFromPath(image)) != TargetKind::Device);
  CHECK(ClassifyTarget(WideFromPath(absent)) == TargetKind::Missing);
}

#ifdef _WIN32

TEST_CASE("ClassifyTarget takes a drive and a device path on Windows",
          "[device-access][device-path]") {
  using NtfsDevice::ClassifyTarget;
  using NtfsDevice::TargetKind;

  CHECK(ClassifyTarget(L"c") == TargetKind::Drive);
  CHECK(ClassifyTarget(L"C:") == TargetKind::Drive);
  CHECK(ClassifyTarget(L"c:\\") == TargetKind::Drive);
  CHECK(ClassifyTarget(L"c:/") == TargetKind::Drive);
  CHECK(ClassifyTarget(L"\\\\.\\PhysicalDrive0") == TargetKind::Device);
  CHECK(ClassifyTarget(L"\\\\.\\C:") == TargetKind::Device);
  CHECK(ClassifyTarget(L"c:\\ntfs-bench-test-absent\\img.bin") ==
        TargetKind::Missing);
}

#else

TEST_CASE("ClassifyTarget takes a file with a colon in its name for a file",
          "[device-access][device-path]") {
  const TempDir dir;
  const std::filesystem::path image = dir.Write("c:", "x");

  CHECK(NtfsDevice::ClassifyTarget(WideFromPath(image)) ==
        NtfsDevice::TargetKind::File);
}

#endif  // _WIN32

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

TEST_CASE("SplitDeviceTarget finds an image at a path of more than 260 units",
          "[device-access][device-path][long-path]") {
  const NTFS_BROWSER_NAMESPACE::Tests::LongTempDir directory(300);
  const std::filesystem::path image =
      directory.Write("disk.img", std::vector<BYTE>(512, 0));
  REQUIRE(image.native().size() > 260);

  const auto whole = SplitDeviceTarget(WideFromPath(image));
  const auto below = SplitDeviceTarget(Target(image, L"Windows/System32"));

  REQUIRE(whole.has_value());
  CHECK(whole->device == image);
  CHECK(whole->directory.empty());
  REQUIRE(below.has_value());
  CHECK(below->device == image);
  CHECK(below->directory == L"Windows\\System32");
}

TEST_CASE("SplitDeviceTarget takes a directory deeper than 260 units",
          "[device-access][device-path][long-path]") {
  const NTFS_BROWSER_NAMESPACE::Tests::LongTempDir directory(300);
  const std::filesystem::path image =
      directory.Write("disk.img", std::vector<BYTE>(512, 0));
  const std::wstring deep(250, L'd');

  const auto target =
      SplitDeviceTarget(Target(image, deep + L"/" + deep + L"/" + deep));

  REQUIRE(target.has_value());
  CHECK(target->device == image);
  CHECK(target->directory == deep + L"\\" + deep + L"\\" + deep);
}

#ifdef _WIN32

TEST_CASE("SplitDeviceTarget takes an extended-length path",
          "[device-access][device-path][long-path]") {
  const NTFS_BROWSER_NAMESPACE::Tests::LongTempDir directory(300);
  const std::filesystem::path image =
      directory.Write("disk.img", std::vector<BYTE>(512, 0));
  const std::wstring extended = LR"(\\?\)" + image.wstring();

  const auto target = SplitDeviceTarget(extended + L"\\Docs");

  REQUIRE(target.has_value());
  CHECK(target->device == std::filesystem::path(extended));
  CHECK(target->directory == L"Docs");
}

TEST_CASE("SplitDeviceTarget takes forward slashes in a long path",
          "[device-access][device-path][long-path]") {
  const NTFS_BROWSER_NAMESPACE::Tests::LongTempDir directory(300);
  const std::filesystem::path image =
      directory.Write("disk.img", std::vector<BYTE>(512, 0));
  std::wstring forward = image.wstring();
  std::ranges::replace(forward, L'\\', L'/');

  const auto target = SplitDeviceTarget(forward + L"/Docs");

  REQUIRE(target.has_value());
  CHECK(target->device == std::filesystem::path(forward));
  CHECK(target->directory == L"Docs");
}

#endif  // _WIN32
