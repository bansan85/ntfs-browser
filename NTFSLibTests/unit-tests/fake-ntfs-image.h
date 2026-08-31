#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <windows.h>

namespace NtfsBrowserTests
{

// $MFT's DATA attribute real_size, encoded as (kSentinelRecordCount *
// kFakeFileRecordSize) bytes. Tests check GetRecordsCount() against this to
// detect if $MFT's own attribute header got silently corrupted.
inline constexpr uint64_t kSentinelRecordCount = 5;

// Every record in the fake image is exactly this size (required by
// FileRecordHeader's own internal assert).
inline constexpr uint32_t kFakeFileRecordSize = 1024;

// Builds a minimal synthetic NTFS-like volume image in memory: a boot sector
// plus $MFT (#0, a non-resident DATA attribute reporting
// kSentinelRecordCount records), $Volume (#3, VOLUME_INFORMATION v3.1) and
// the root directory (#5, header only, no attributes) file records. Just
// enough for NtfsVolume<S> to open successfully, whether read from memory or
// from a file holding these same bytes.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImage();

// Same image as BuildFakeNtfsImage(), written to a temp file.
//
// Returns the path NtfsVolume<S> should be opened with.
[[nodiscard]] std::filesystem::path WriteFakeNtfsImage();

// MFT index of the directory record built by
// BuildFakeNtfsImageWithAttributeListDirectory(): its only attribute is a
// resident $ATTRIBUTE_LIST relocating $INDEX_ROOT to kIndexExtensionIdx.
inline constexpr ULONGLONG kAttributeListDirIdx = 6;

// MFT index of the extension record holding the $INDEX_ROOT that
// kAttributeListDirIdx's $ATTRIBUTE_LIST points to. Its single named entry
// is "Foo" (file reference 20).
inline constexpr ULONGLONG kIndexExtensionIdx = 7;

// Same volume as BuildFakeNtfsImage(), plus a directory split across two
// records the way real, non-trivial NTFS directories (eg. C:\Windows) can
// be: the base record (kAttributeListDirIdx) holds only an
// $ATTRIBUTE_LIST, and the actual $INDEX_ROOT - a single "Foo" entry -
// lives in the extension record it points to (kIndexExtensionIdx).
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttributeListDirectory();

}  // namespace NtfsBrowserTests
