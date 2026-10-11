#pragma once

#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <memory>
#include <tuple>

#include <ntfs-browser/disk-reader.h>

namespace NtfsBrowserTests {

// Roots of the forensic image corpora, outside this repo. CMake sets each one
// from its NTFS_BROWSER_TEST_* cache or environment variable, as a UTF-8 path.
// .github/scripts/fetch-test-data.py builds the default layout.

// DFTT (dftt.sourceforge.net): one subdirectory per unzipped test.
inline const std::filesystem::path& DfttDir() {
  static const std::filesystem::path path = u8"" NTFS_TEST_DFTT_DIR;
  return path;
}

// Digital Corpora nps-2009-ntfs1: ntfs1-gen2.raw, converted from its .E01.
inline const std::filesystem::path& NpsNtfs1Dir() {
  static const std::filesystem::path path = u8"" NTFS_TEST_NPS_NTFS1_DIR;
  return path;
}

// github.com/msuhanov/ntfs-samples, each *.raw.gz decompressed in place.
inline const std::filesystem::path& NtfsSamplesDir() {
  static const std::filesystem::path path = u8"" NTFS_TEST_NTFS_SAMPLES_DIR;
  return path;
}

// ntfs_extremely_fragmented_mft.raw: 256 GiB, so it may live elsewhere.
inline const std::filesystem::path& FragmentedMftImage() {
  static const std::filesystem::path path = u8"" NTFS_TEST_FRAGMENTED_MFT_IMAGE;
  return path;
}

// SKIPs the running test when image is absent. Under
// NTFS_BROWSER_REQUIRE_TEST_DATA, FAILs it instead: CI fetches every image
// this guards, so a skip there would hide a broken fetch.
void RequireCorpusImage(const std::filesystem::path& image);

// Opens imagePath (a bare-volume image: no MBR, boot sector at byte 0)
// through a PartitionDiskReader, so NtfsVolume's own path-based constructor -
// Windows-only, since it goes through Win32DiskReader - is never needed to
// open a corpus image.
[[nodiscard]] std::unique_ptr<NtfsBrowser::IDiskReader>
    OpenBareVolumeImage(const std::filesystem::path& image_path);

// Combines a FILETIME's two 32-bit halves into its 100 ns tick count since
// 1601-01-01.
[[nodiscard]] ULONGLONG FileTimeToTicks(const FILETIME& file_time) noexcept;

// Decomposes ft into a Gregorian (year, month, day) triple. A portable stand-
// in for Win32's FileTimeToSystemTime(), which does not exist off Windows.
[[nodiscard]] std::tuple<WORD, WORD, WORD>
    FileTimeToDate(const FILETIME& file_time) noexcept;

}  // namespace NtfsBrowserTests
