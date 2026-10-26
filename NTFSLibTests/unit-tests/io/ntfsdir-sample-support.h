#pragma once

#include <ntfs-browser/win-types.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <ntfs-browser/namespace.h>

#include "fake-ntfs-image.h"
#include "local-time.h"
#include "sample-run.h"

// What the tests of NtfsDir share: how to run it, the image it lists, and the
// text it prints.
namespace NTFS_BROWSER_NAMESPACE::Tests::DirSample {

// The samples under test: NtfsDir on every platform, and NtfsDir2, the same
// listing over the other cache strategy, where it is built.
inline std::vector<std::filesystem::path> SampleExes() {
#ifdef _WIN32
  return {NTFS_DIR_EXE, NTFS_DIR2_EXE};
#else
  return {NTFS_DIR_EXE};
#endif
}

// Runs one sample with args, after checking that it was built.
inline SampleRun RunDir(const std::filesystem::path& exe,
                        const std::vector<std::wstring>& args) {
  REQUIRE(std::filesystem::exists(exe));
  return RunSample(exe, args);
}

// Writes an NTFS image to a temp file, removed on exit. The default holds
// "Docs" in the root and "report.txt" in Docs, with a known write time.
struct TempImage final {
  std::filesystem::path path;

  TempImage() : TempImage(L"report.txt") {}

  explicit TempImage(std::wstring_view report_name)
      : TempImage(BuildFakeNtfsImageWithReportName(report_name,
                                                   dump_broken_alter_time)) {}

  explicit TempImage(const std::vector<BYTE>& image)
      : path(WriteFakeNtfsImage(image)) {}

  ~TempImage() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  TempImage(const TempImage&) = delete;
  TempImage& operator=(const TempImage&) = delete;
  TempImage(TempImage&&) = delete;
  TempImage& operator=(TempImage&&) = delete;
};

// The listing line of an entry of size zero and write time ticks (UTC), as
// NtfsDir prints it in the host's time zone. Empty where the platform cannot
// show that time, which leaves the line out. marker is the --deleted column
// before the name, or empty when the listing has none.
inline std::string ListingLine(std::string_view name, bool directory,
                               ULONGLONG ticks, std::string_view marker = {}) {
  const auto local = NtfsDevice::UtcTicksToLocalTime(ticks);
  if (!local) {
    return {};
  }

  char text[160]{};
  snprintf(text, sizeof(text), "%u-%02u-%02u  %02u:%02u\t%s    %s<   >\t",
           local->year, local->month, local->day, local->hour, local->minute,
           directory ? "<DIR>" : "     ", directory ? "\t" : "0\t");
  const std::string marker_column =
      marker.empty() ? std::string() : std::string(marker) + " ";
  return std::string(text) + marker_column + std::string(name) + "\n";
}

// The listing of the root of a TempImage: only "Docs", whose index entry has
// no write time.
inline std::string RootListing() {
  return ListingLine("Docs", true, 0) + "Files: 0, Directories: 1\n";
}

// The listing of Docs in a TempImage: one file called name. marker is the
// --deleted column, or empty.
inline std::string DocsListing(std::string_view name,
                               std::string_view marker = {}) {
  return ListingLine(name, false, dump_broken_alter_time, marker) +
         "Files: 1, Directories: 0\n";
}

// The line that opens the block of directory in a recursive listing.
inline std::string DirectoryHeader(std::string_view directory) {
  return "\nDirectory of " + std::string(directory) + "\n";
}

// directory with name below it, as a recursive listing writes a path.
inline std::string Below(const std::string& directory, std::string_view name) {
  return directory +
         static_cast<char>(std::filesystem::path::preferred_separator) +
         std::string(name);
}

// The line a recursive listing writes where it does not enter path.
inline std::string SkipLine(std::string_view path) {
  return "Skipping " + std::string(path) + ": directory already listed\n";
}

// The path of image with directory after it, in the form the samples take.
inline std::wstring InImage(const std::filesystem::path& image,
                            std::wstring_view directory) {
  return image.wstring() + L"/" + std::wstring(directory);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests::DirSample
