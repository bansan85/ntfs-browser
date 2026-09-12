#pragma once

#include <cstdint>
#include <filesystem>

namespace NtfsBrowserTests
{

// Fake record count $MFT reports; arbitrary, tests just check it survives.
inline constexpr uint64_t kSentinelRecordCount = 5;

// Every fake record's size; FileRecordHeader asserts on this size internally.
inline constexpr uint32_t kFakeFileRecordSize = 1024;

// Writes a minimal fake NTFS volume to a temp file and returns its path.
[[nodiscard]] std::filesystem::path WriteFakeNtfsImage();

}
