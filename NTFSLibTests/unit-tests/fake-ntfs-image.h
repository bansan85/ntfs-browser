#pragma once

#include <array>
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

// clusters_per_index_block patched in by
// BuildFakeNtfsImageWithTinyIndexBlock(); read as a signed char, 0xFF
// becomes -1, producing a far too small index_block_size_.
inline constexpr BYTE kTinyClustersPerIndexBlock = 0xFF;

// index_block_size_ that kTinyClustersPerIndexBlock is expected to produce.
inline constexpr DWORD kTinyIndexBlockSize = 2;

// Same volume as BuildFakeNtfsImage(), with clusters_per_index_block patched
// so GetIndexBlockSize() is far smaller than Data::IndexBlock's own header.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithTinyIndexBlock();

// MFT index of the directory record built by
// BuildFakeNtfsImageWithMultiTypeAttributeListDirectory().
inline constexpr ULONGLONG kAttrListMultiTypeDirIdx = 10;

// MFT index of the extension record kAttrListMultiTypeDirIdx's
// $ATTRIBUTE_LIST points at for both entries.
inline constexpr ULONGLONG kMultiTypeExtensionIdx = 11;

// Same volume as BuildFakeNtfsImage(), plus a directory whose
// $ATTRIBUTE_LIST relocates two different attribute types ($INDEX_ROOT
// and $INDEX_ALLOCATION) into the same extension record.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMultiTypeAttributeListDirectory();

// MFT index of a second directory record built by
// BuildFakeNtfsImageWithAttributeListDirectoryChainReused().
inline constexpr ULONGLONG kAttributeListDirIdx2 = 12;

// Same volume as BuildFakeNtfsImageWithAttributeListDirectory(), plus a
// second, independent directory record whose $ATTRIBUTE_LIST also
// relocates $INDEX_ROOT to the same extension record the first one uses.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithAttributeListDirectoryChainReused();

// MFT index of the directory record built by
// BuildFakeNtfsImageWithFragmentedAttributeListDirectory().
inline constexpr ULONGLONG kUafAttrListDirIdx = 13;

// MFT indices of the four extension records that directory's
// $ATTRIBUTE_LIST relocates $INDEX_ALLOCATION to. Must stay below
// Enum::MftIdx::USER (16); kUafExtensionIdx2/3 reuse indices 1 and 2,
// left unused by BuildFakeNtfsImage().
inline constexpr ULONGLONG kUafExtensionIdx0 = 14;
inline constexpr ULONGLONG kUafExtensionIdx1 = 15;
inline constexpr ULONGLONG kUafExtensionIdx2 = 1;
inline constexpr ULONGLONG kUafExtensionIdx3 = 2;

// Distinct real_size sentinel written into each extension record's
// $INDEX_ALLOCATION, so a test can tell which record's memory it is
// still reading.
inline constexpr std::array<DWORD, 4> kUafRealSizeSentinels{1024, 2048, 3072,
                                                            4096};

// Same volume as BuildFakeNtfsImage(), plus a directory whose
// $ATTRIBUTE_LIST relocates $INDEX_ALLOCATION into four distinct
// extension records.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithFragmentedAttributeListDirectory();

}  // namespace NtfsBrowserTests
