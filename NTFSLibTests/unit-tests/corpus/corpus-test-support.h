#pragma once

#include <ntfs-browser/win-types.h>

#include <filesystem>
#include <memory>
#include <string_view>
#include <tuple>

#include <ntfs-browser/disk-reader.h>
#include <ntfs-browser/namespace.h>

namespace NTFS_BROWSER_NAMESPACE::Tests {

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

// Folder of the recorded read traces, in the fuzzer's data folder. A trace is
// the bytes a test read from an image, one read after the other and without
// their address: SequentialDiskReader replays it, so a test can run without
// the (often huge) image. Set NTFS_BROWSER_RECORD_TRACES to rewrite the
// traces of every test whose image is present. Set NTFS_BROWSER_FORCE_TRACES
// to replay them even then, to check that they still match the library's
// read order.
inline const std::filesystem::path& TraceDir() {
  static const std::filesystem::path path = u8"" NTFS_TEST_TRACE_DIR;
  return path;
}

// SKIPs the running test when it has neither image nor the trace named
// trace_name. Under NTFS_BROWSER_REQUIRE_TEST_DATA, FAILs it instead when
// image is absent: CI fetches every image this guards, so a trace standing in
// for it there would hide a broken fetch.
void RequireCorpusImage(const std::filesystem::path& image,
                        std::string_view trace_name);

// Same for an image CI never fetches: its test only SKIPs, whatever
// NTFS_BROWSER_REQUIRE_TEST_DATA says.
void RequireUnfetchedCorpusImage(const std::filesystem::path& image,
                                 std::string_view trace_name);

// SKIPs the running test when image is absent. Under
// NTFS_BROWSER_REQUIRE_TEST_DATA, FAILs it instead. For a test with no trace.
void RequireCorpusImage(const std::filesystem::path& image);

// Opens image through a PartitionDiskReader, offsetting every read by
// partition_offset (0 for a bare-volume image: no MBR, boot sector at byte
// 0). NtfsVolume's own path-based constructor - Windows-only, since it goes
// through Win32DiskReader - is therefore never needed to open a corpus image.
// Without image, or under NTFS_BROWSER_FORCE_TRACES, it replays the trace
// named trace_name instead. Under NTFS_BROWSER_RECORD_TRACES, it records that
// trace while reading image.
[[nodiscard]] std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>
    OpenCorpusImage(const std::filesystem::path& image,
                    ULONGLONG partition_offset, std::string_view trace_name);

// Opens image as OpenCorpusImage() does, but never from a trace. For a test
// with no trace.
[[nodiscard]] std::unique_ptr<NTFS_BROWSER_NAMESPACE::IDiskReader>
    OpenBareVolumeImage(const std::filesystem::path& image_path);

// Combines a FILETIME's two 32-bit halves into its 100 ns tick count since
// 1601-01-01.
[[nodiscard]] ULONGLONG FileTimeToTicks(const FILETIME& file_time) noexcept;

// Decomposes ft into a Gregorian (year, month, day) triple. A portable stand-
// in for Win32's FileTimeToSystemTime(), which does not exist off Windows.
[[nodiscard]] std::tuple<WORD, WORD, WORD>
    FileTimeToDate(const FILETIME& file_time) noexcept;

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
