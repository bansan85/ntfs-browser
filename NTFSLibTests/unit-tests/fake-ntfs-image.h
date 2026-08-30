#pragma once

#include <cstdint>
#include <filesystem>

namespace NtfsBrowserTests
{

// $MFT's DATA attribute real_size, encoded as (kSentinelRecordCount *
// kFakeFileRecordSize) bytes. Tests check GetRecordsCount() against this to
// detect if $MFT's own attribute header got silently corrupted.
inline constexpr uint64_t kSentinelRecordCount = 5;

// Every record in the fake image is exactly this size (required by
// FileRecordHeader's own internal assert).
inline constexpr uint32_t kFakeFileRecordSize = 1024;

// Writes a minimal synthetic NTFS-like volume image to a temp file: a boot
// sector plus $MFT (#0, a non-resident DATA attribute reporting
// kSentinelRecordCount records), $Volume (#3, VOLUME_INFORMATION v3.1) and
// the root directory (#5, header only, no attributes) file records. Just
// enough for NtfsVolume<S>(path) to open successfully.
//
// Returns the path NtfsVolume<S> should be opened with.
[[nodiscard]] std::filesystem::path WriteFakeNtfsImage();

}  // namespace NtfsBrowserTests
