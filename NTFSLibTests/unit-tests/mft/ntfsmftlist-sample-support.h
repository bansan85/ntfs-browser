#pragma once

#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "fake-ntfs-image.h"
#include "sample-run.h"

// What the tests of NtfsMftList share: how to run it, and the image it lists.
namespace NTFS_BROWSER_NAMESPACE::Tests::MftListSample {

// Runs NtfsMftList with args, its stdout and stderr captured apart.
inline SampleRun RunMftList(const std::vector<std::wstring>& args) {
  const std::filesystem::path exe(NTFS_MFT_LIST_EXE);
  REQUIRE(std::filesystem::exists(exe));
  return RunSample(exe, args);
}

// Writes an MFT tree image to a temp file, removed on exit. The default is
// BuildFakeNtfsImageWithMftTree().
struct TempTreeImage final {
  std::filesystem::path path;

  TempTreeImage() : TempTreeImage(BuildFakeNtfsImageWithMftTree()) {}

  explicit TempTreeImage(const std::vector<BYTE>& image)
      : path(WriteFakeNtfsImage(image)) {}

  ~TempTreeImage() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  TempTreeImage(const TempTreeImage&) = delete;
  TempTreeImage& operator=(const TempTreeImage&) = delete;
  TempTreeImage(TempTreeImage&&) = delete;
  TempTreeImage& operator=(TempTreeImage&&) = delete;
};

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::MftListSample
