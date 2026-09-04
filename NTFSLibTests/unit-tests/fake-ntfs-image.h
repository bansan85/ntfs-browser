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

// MFT index of the record built by
// BuildFakeNtfsImageWithUndersizedAttribute(): its only attribute declares
// total_size = 17, smaller than sizeof(Attr::HeaderResident) (24).
inline constexpr ULONGLONG kUndersizedAttrRecordIdx = 8;

// Same volume as BuildFakeNtfsImage(), plus a record (kUndersizedAttrRecordIdx)
// whose single attribute's total_size (17 bytes) is too small to hold even a
// resident attribute's fixed header (24 bytes) - regression fixture for F1:
// FileRecord::ParseAttrs() used to never check total_size against the header
// size it is about to reinterpret_cast, so AttrResident{No,Full}Cache's ctor
// read attr_size/attr_offset from bytes past the attribute's declared extent.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithUndersizedAttribute();

// MFT index of the directory record built by
// BuildFakeNtfsImageWithForgedIndexBlock(): its $INDEX_ROOT holds a single,
// nameless SUBNODE-only entry pointing at VCN 0 of its $INDEX_ALLOCATION,
// whose sole index block is the forged one described below.
inline constexpr ULONGLONG kIndexAllocDirIdx = 9;

// Size (bytes) of the single index block kIndexAllocDirIdx's
// $INDEX_ALLOCATION describes - AttrIndexAlloc<S>::ParseIndexBlock()
// allocates exactly this many bytes (IndexBlock::AllocIndexBlock,
// src/index-block.cpp) to hold it. A whole number of the fixture's 1KB
// clusters (7), chosen distinct from every other allocation size in this
// fixture so a test-only allocator hook can recognize it unambiguously.
inline constexpr DWORD kForgedIndexBlockSize = 7 * kFakeFileRecordSize;

// offset_of_us the forged index block declares (Data::IndexBlock,
// src/data/index-block.h) - regression fixture for F4:
// AttrIndexAlloc<S>::ParseIndexBlock() (src/attr-index-alloc.cpp) never
// checks offset_of_us against the index_block_size-byte buffer it just
// allocated before reading the Update Sequence Number through it. 0xFFFF
// (the maximum a WORD can hold) is far past kForgedIndexBlockSize, matching
// the bug report's own scenario.
inline constexpr WORD kForgedIndexBlockOffsetOfUs = 0xFFFF;

// Same volume as BuildFakeNtfsImage(), plus a directory (kIndexAllocDirIdx)
// whose $INDEX_ALLOCATION's single data run points at an "INDX"-signed
// index block whose offset_of_us (kForgedIndexBlockOffsetOfUs) is never
// validated against the block's real size (kForgedIndexBlockSize) - see F4
// in docs/bug-reports/2026-09-03-full-repo.md.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithForgedIndexBlock();

// clusters_per_index_block value BuildFakeNtfsImageWithTinyIndexBlock()
// patches into the BPB - regression fixture for F6:
// NtfsVolume<S>::ParseBootSector() (src/ntfs-volume.cpp) reads
// clusters_per_index_block as a signed char, so 0xFF -> -1 -> index_block_size_
// = 1 << 1 = 2 bytes, without ever comparing the result against
// sizeof(Data::IndexBlock) (40 bytes) - the structure every index block is
// about to be allocated and parsed as (src/attr-index-alloc.cpp).
inline constexpr BYTE kTinyClustersPerIndexBlock = 0xFF;

// index_block_size_ that kTinyClustersPerIndexBlock is expected to produce.
inline constexpr DWORD kTinyIndexBlockSize = 2;

// Same volume as BuildFakeNtfsImage(), with clusters_per_index_block patched
// to kTinyClustersPerIndexBlock so GetIndexBlockSize() ends up
// kTinyIndexBlockSize bytes - far too small to even hold Data::IndexBlock's
// own 40-byte header - yet NtfsVolume<S>::ParseBootSector() currently accepts
// the volume regardless. See F6 in docs/bug-reports/2026-09-03-full-repo.md.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithTinyIndexBlock();

}  // namespace NtfsBrowserTests
