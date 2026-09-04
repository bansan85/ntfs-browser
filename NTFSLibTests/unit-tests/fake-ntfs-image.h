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

// Directory record index: only attribute is a resident $ATTRIBUTE_LIST
// relocating $INDEX_ROOT to kIndexExtensionIdx.
inline constexpr ULONGLONG kAttributeListDirIdx = 6;

// Extension record index: holds the $INDEX_ROOT kAttributeListDirIdx's
// $ATTRIBUTE_LIST points to, with a single named entry "Foo" (ref 20).
inline constexpr ULONGLONG kIndexExtensionIdx = 7;

// Same volume as BuildFakeNtfsImage(), plus a directory split across two
// records the way real NTFS directories (eg. C:\Windows) can be: the base
// record holds only $ATTRIBUTE_LIST, and $INDEX_ROOT lives in the
// extension record it points to.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttributeListDirectory();

// MFT index of the record built by
// BuildFakeNtfsImageWithUndersizedAttribute(): its only attribute declares
// total_size = 17, smaller than sizeof(Attr::HeaderResident) (24).
inline constexpr ULONGLONG kUndersizedAttrRecordIdx = 8;

// Same volume as BuildFakeNtfsImage(), plus a record (kUndersizedAttrRecordIdx)
// whose single attribute is smaller than a resident attribute's own fixed
// header - regression fixture for an attribute whose size/offset fields get
// read from bytes past its own declared extent.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithUndersizedAttribute();

// MFT index of the directory record built by
// BuildFakeNtfsImageWithForgedIndexBlock(); the next unused index.
inline constexpr ULONGLONG kIndexAllocDirIdx = 9;

// Size (bytes) of the forged index block: a whole number of clusters,
// kept distinct from every other allocation size in this file.
inline constexpr DWORD kForgedIndexBlockSize = 7 * kFakeFileRecordSize;

// offset_of_us declared by the forged index block: far past
// kForgedIndexBlockSize, overrunning the block's buffer.
inline constexpr WORD kForgedIndexBlockOffsetOfUs = 0xFFFF;

// Same volume as BuildFakeNtfsImage(), plus a directory whose
// $INDEX_ALLOCATION points at an index block with an out-of-bounds
// offset_of_us.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithForgedIndexBlock();

}  // namespace NtfsBrowserTests
