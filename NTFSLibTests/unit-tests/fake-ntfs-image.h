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

// True on-disk size of $VOLUME_INFORMATION: 12 bytes, with no padding.
inline constexpr WORD kMinimalVolumeInformationSize = 12;

// Same volume as BuildFakeNtfsImage(), with $Volume's (#3) VOLUME_INFORMATION
// attribute shrunk to kMinimalVolumeInformationSize bytes.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMinimalVolumeInformation();

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

// clusters_per_index_block patched in by
// BuildFakeNtfsImageWithOversizedIndexBlock(); read as a signed char, 0xE1
// becomes -31, producing an index_block_size_ far larger than any real
// volume's, yet still divisible by every common sector size.
inline constexpr BYTE kOversizedClustersPerIndexBlock = 0xE1;

// index_block_size_ that kOversizedClustersPerIndexBlock is expected to
// produce.
inline constexpr DWORD kOversizedIndexBlockSize = 0x80000000;

// Same volume as BuildFakeNtfsImage(), with clusters_per_index_block patched
// so GetIndexBlockSize() is far larger than any real volume's, while still
// passing every existing bound check.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithOversizedIndexBlock();

// clusters_per_file_record patched in by
// BuildFakeNtfsImageWithOversizedFileRecord(); same 0xE1 shift as
// kOversizedClustersPerIndexBlock, but for file_record_size_.
inline constexpr BYTE kOversizedClustersPerFileRecord = 0xE1;

// file_record_size_ that kOversizedClustersPerFileRecord is expected to
// produce.
inline constexpr DWORD kOversizedFileRecordSize = 0x80000000;

// Same volume as BuildFakeNtfsImage(), with clusters_per_file_record patched
// so GetFileRecordSize() is far larger than any real volume's. Unlike the
// index-block variant above, this size is read the moment any file record
// is parsed, so this fixture is not driven through a full NtfsVolume
// construction in tests.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithOversizedFileRecord();

// Yields a file_record_size_ twice FileRecordHeader::kMaxFileRecordSize.
inline constexpr BYTE kFileRecordSizeTooBigClustersPerFileRecord = 8;

// file_record_size_ that kFileRecordSizeTooBigClustersPerFileRecord produces.
inline constexpr DWORD kFileRecordSizeTooBig = 8192;

// Same volume as BuildFakeNtfsImage(), with clusters_per_file_record patched
// so GetFileRecordSize() would exceed FileRecordHeader::kMaxFileRecordSize.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithFileRecordSizeTooBig();

// lcn_mft patched in by BuildFakeNtfsImageWithHugeMftLcn(); with this
// fixture's fixed cluster size, mft_addr_ ends up exactly 2^63.
inline constexpr ULONGLONG kHugeMftLcn = 1ULL << 53;

// Same volume as BuildFakeNtfsImage(), with lcn_mft patched to kHugeMftLcn
// so mft_addr_ ends up too large for a LONGLONG.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithHugeMftLcn();

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

// Same volume as BuildFakeNtfsImage(), but with the $MFT file record
// zero-filled (magic == 0, not kFileRecordMagic), while $Volume and the
// root directory are left untouched and valid.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithCorruptMftRecord();

// MFT index of the record built by
// BuildFakeNtfsImageWithAttrNameExceedsTotalSize(). Free below
// Enum::MftIdx::USER (16).
inline constexpr ULONGLONG kAttrNameExceedsTotalSizeRecordIdx = 4;

// name_offset/name_length BuildFakeNtfsImageWithAttrNameExceedsTotalSize()
// forges for its single $DATA attribute: together they reach past the
// attribute's declared total_size (28), while still landing well inside
// the 1024-byte record buffer.
inline constexpr WORD kAttrNameBoundsNameOffset = 100;
inline constexpr BYTE kAttrNameBoundsNameLength = 6;

// Deterministic bytes written at kAttrNameBoundsNameOffset, exactly
// kAttrNameBoundsNameLength wide characters (excluding the terminator).
inline constexpr wchar_t kAttrNameBoundsSentinel[] = L"PWNED!";

// Same volume as BuildFakeNtfsImage(), plus a record whose single resident
// $DATA attribute declares a name reaching past its own total_size, while
// still landing on known, deterministic bytes.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithAttrNameExceedsTotalSize();

// Well past this fixture's 1024-byte record, but within a WORD's range.
inline constexpr WORD kAttrOffsetOutOfBounds = 2000;

// Same volume as BuildFakeNtfsImage(), with the root directory's (#5)
// offset_of_attr patched to kAttrOffsetOutOfBounds.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttrOffsetOutOfBounds();

// Recognizable byte pattern written as this fixture's entire $DATA body.
inline constexpr std::array<BYTE, 4> kSmallResidentDataContent{0xDE, 0xAD, 0xBE,
                                                               0xEF};

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose sole attribute is a resident $DATA holding exactly
// kSmallResidentDataContent.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithSmallResidentData();

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose resident $ATTRIBUTE_LIST ends in a short read.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttributeListShortRead();

// MFT index of the second record in a two-way $ATTRIBUTE_LIST cycle with #5.
inline constexpr ULONGLONG kAttrListCycleExtIdx = 6;

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// and kAttrListCycleExtIdx each replaced by one whose resident
// $ATTRIBUTE_LIST names the other, forming a resolution cycle.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttributeListCycle();

// Real, fixed on-disk size of a nameless $ATTRIBUTE_LIST entry's header.
inline constexpr WORD kAttributeListRealEntrySize = 26;

// MFT index of the directory record with a densely-packed $ATTRIBUTE_LIST.
inline constexpr ULONGLONG kAttrListTightPackDirIdx = 6;

// MFT index kAttrListTightPackDirIdx's $ATTRIBUTE_LIST relocates
// $INDEX_ROOT to.
inline constexpr ULONGLONG kAttrListTightPackExtIdxA = 7;

// MFT index kAttrListTightPackDirIdx's $ATTRIBUTE_LIST relocates
// $INDEX_ALLOCATION to.
inline constexpr ULONGLONG kAttrListTightPackExtIdxB = 8;

// real_size written into kAttrListTightPackExtIdxB's $INDEX_ALLOCATION.
inline constexpr DWORD kAttrListTightPackRealSize = 4096;

// Same volume as BuildFakeNtfsImage(), plus a directory whose resident
// $ATTRIBUTE_LIST packs two entries at the real on-disk entry stride.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory();

// Same volume as BuildFakeNtfsImage(), with the root directory's (#5) file
// record zero-filled so ParseFileRecord(ROOT) fails.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithCorruptRootRecord();

}  // namespace NtfsBrowserTests
