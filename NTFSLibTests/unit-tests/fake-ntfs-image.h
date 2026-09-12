#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <windows.h>

namespace NtfsBrowserTests
{

// Fake record count $MFT reports; arbitrary, tests just check it survives.
inline constexpr uint64_t kSentinelRecordCount = 5;

// Every fake record's size; FileRecordHeader asserts on this size internally.
inline constexpr uint32_t kFakeFileRecordSize = 1024;

// Builds a minimal fake NTFS volume image in memory: boot sector, $MFT,
// $Volume, and root directory records, just enough for NtfsVolume<S> to
// open it.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImage();

// Writes BuildFakeNtfsImage()'s image to a temp file and returns its path.
[[nodiscard]] std::filesystem::path WriteFakeNtfsImage();

}
