#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include "gap-collation-probe.h"
#include "named-stream-probe.h"

namespace NtfsBrowserTests
{

// Fake record count $MFT reports; arbitrary, tests just check it survives.
inline constexpr uint64_t kSentinelRecordCount = 5;

// Every fake record's size; FileRecordHeader asserts on this size internally.
inline constexpr uint32_t kFakeFileRecordSize = 1024;

// Volume geometry every image built here declares in its BPB: one sector
// per file record and one sector per cluster.
inline constexpr WORD kFakeBytesPerSector = kFakeFileRecordSize;
inline constexpr BYTE kFakeSectorsPerCluster = 1;
// Use this, not kFakeFileRecordSize, for anything sized in clusters.
inline constexpr DWORD kFakeClusterSize =
    static_cast<DWORD>(kFakeBytesPerSector) * kFakeSectorsPerCluster;

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

// Volume name BuildFakeNtfsImageWithVolumeName() stores in $Volume's
// VOLUME_NAME attribute. ASCII, so its UTF-8 form is the same bytes.
inline constexpr std::wstring_view kFakeVolumeName = L"TESTVOL";

// Same volume as BuildFakeNtfsImage(), plus a VOLUME_NAME attribute
// holding kFakeVolumeName on $Volume (#3).
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithVolumeName();

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

// Smallest MFT index only reachable through ReadFileRecord()'s
// fragmented-$MFT path.
inline constexpr ULONGLONG kFragmentedMftInvalidRecordIdx = 16;

// Physical LCN where the forged fragmented-$MFT record is placed.
inline constexpr DWORD kFragmentedMftDataRunLcn = 20;

// Same volume as BuildFakeNtfsImage(), except $MFT's DATA attribute has a
// real data run reaching kFragmentedMftInvalidRecordIdx, whose file record
// is forged with an invalid offset_of_us.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithFragmentedMftInvalidRecord();

// Shared with NTFSLibTests/fuzz/named-stream-probe.h, so a fuzz corpus file
// built from this fixture reaches the same named stream by name.
using NtfsFuzz::kNamedDataStreamName;
using NtfsFuzz::kNamedDataStreamNameLength;

// Recognizable byte pattern written as this fixture's entire $DATA body.
inline constexpr std::array<BYTE, 4> kNamedDataStreamContent{0xCA, 0xFE, 0xBA,
                                                             0xBE};

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose sole attribute is a named $DATA stream.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithNamedDataStream();

// MFT indices of the two directory records built by
// BuildFakeNtfsImageWithIndexRootVariants().
inline constexpr ULONGLONG kIndexRootVariantADirIdx = 6;
inline constexpr ULONGLONG kIndexRootVariantBDirIdx = 7;

// mft_index each variant's single FILE_NAME entry declares.
inline constexpr ULONGLONG kIndexRootVariantAMftRef = 30;
inline constexpr ULONGLONG kIndexRootVariantBMftRef = 40;

// File names each variant's single FILE_NAME entry declares.
inline constexpr wchar_t kIndexRootVariantAName[] = L"AAA";
inline constexpr wchar_t kIndexRootVariantBName[] = L"BBB";

// Same volume as BuildFakeNtfsImage(), plus two same-size directory records,
// each holding its own resident $INDEX_ROOT with a distinct FILE_NAME entry.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithIndexRootVariants();

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one holding its own real $INDEX_ROOT entry directly, not via
// an $ATTRIBUTE_LIST extension record.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithRootIndexRootEntry();

// Shared with NTFSLibTests/fuzz/gap-collation-probe.h, so a fuzz corpus
// file built from this fixture reaches the same sub-node entry by name.
using NtfsFuzz::kGapCollationSearchName;
using NtfsFuzz::kGapCollationSearchNameLength;

// MFT reference the root-level, non-terminal $INDEX_ROOT entry declares.
inline constexpr ULONGLONG kGapCollationNonTerminalMftRef = 25;

// MFT reference the sub-node's real leaf entry declares.
inline constexpr ULONGLONG kGapCollationLeafMftRef = 30;

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose own $INDEX_ROOT holds a real, non-terminal entry
// that is also a sub-node pointer into a real $INDEX_ALLOCATION index
// block holding kGapCollationSearchName as its leaf entry.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithGapCollationSubNode();

// Chain length for BuildFakeNtfsImageWithDeepIndexBlockChain(), well past
// the depth limit; every VCN is distinct, so no cycle guard can stop it.
inline constexpr DWORD kIndexBlockChainLength = 70;

// File reference the chain's leaf entry declares.
inline constexpr ULONGLONG kIndexBlockChainLeafMftRef = 99;

// Name (and UTF-16 length) of the chain's leaf entry, reachable only by
// descending past every other block first.
inline constexpr wchar_t kIndexBlockChainLeafName[] = L"Deep";
inline constexpr BYTE kIndexBlockChainLeafNameLength = 4;

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by a bare directory whose $INDEX_ALLOCATION chains
// kIndexBlockChainLength index blocks, each pointing to the next, the last
// holding kIndexBlockChainLeafName as a real entry.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithDeepIndexBlockChain();

// Real on-disk minimum size of a legacy NTFS 1.2 $STANDARD_INFORMATION
// attribute, before the Windows-2000-era owner_id/security_id/quota/usn
// extension appended four more fields.
inline constexpr WORD kLegacyStandardInformationSize = 48;

// MFT index of the record built by
// BuildFakeNtfsImageWithLegacyStandardInformation().
inline constexpr ULONGLONG kLegacyStandardInformationRecordIdx = 6;

// Same volume as BuildFakeNtfsImage(), plus a record
// (kLegacyStandardInformationRecordIdx) whose only attribute is a resident
// $STANDARD_INFORMATION shrunk to kLegacyStandardInformationSize bytes.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLegacyStandardInformation();

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by a bare record whose only attribute is a resident
// $STANDARD_INFORMATION shrunk to kLegacyStandardInformationSize bytes, so
// FuzzOnce() (which only ever parses MftIdx::ROOT) can reach it directly.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLegacyStandardInformationOnRoot();

////////////////////////////////////////////////////////////////////////////
// NTFS compression (FILE_ATTRIBUTE_COMPRESSED + comp_unit_size + LZNT1)
////////////////////////////////////////////////////////////////////////////

// One data run: "clusters" clusters at LCN "lcn", or a sparse hole when
// "lcn" is empty. Encodes into NTFS' real run-list format, so fixtures can
// describe fragmented/sparse layouts. "clusters" must fit one length byte.
struct FakeDataRun
{
  std::optional<DWORD> lcn;
  DWORD clusters;
};

// Compression unit exponent every fixture uses: 4 clusters, one chunk.
inline constexpr WORD kCompressionUnitSizeShift = 2;

// Clusters and bytes per compression unit implied by
// kCompressionUnitSizeShift.
inline constexpr DWORD kCompressionUnitClusters = 1U
                                                  << kCompressionUnitSizeShift;
inline constexpr DWORD kCompressionUnitSize =
    kCompressionUnitClusters * kFakeClusterSize;

// LCN where compression fixtures place real cluster data, clear of the
// older fixtures' index-block LCNs (20, 100).
inline constexpr DWORD kCompressedDataLcn = 30;

// Second, non-contiguous LCN so a fragmented unit's compressed bytes span
// two Data::RunEntrys instead of one.
inline constexpr DWORD kFragmentedCompressedDataLcn = 35;

// [MS-XCA] section 3.3's worked example: 59 bytes of real LZNT1-compressed
// data, decompressing to kXcaLznt1ExampleDecompressed. Used verbatim so no
// compressor need be written in this read-only-library repo.
inline constexpr std::array<BYTE, 59> kXcaLznt1ExampleCompressed{
    0x38, 0xb0, 0x88, 0x46, 0x23, 0x20, 0x00, 0x20, 0x47, 0x20, 0x41, 0x00,
    0x10, 0xa2, 0x47, 0x01, 0xa0, 0x45, 0x20, 0x44, 0x00, 0x08, 0x45, 0x01,
    0x50, 0x79, 0x00, 0xc0, 0x45, 0x20, 0x05, 0x24, 0x13, 0x88, 0x05, 0xb4,
    0x02, 0x4a, 0x44, 0xef, 0x03, 0x58, 0x02, 0x8c, 0x09, 0x16, 0x01, 0x48,
    0x45, 0x00, 0xbe, 0x00, 0x9e, 0x00, 0x04, 0x01, 0x18, 0x90, 0x00};

// The ANSI string kXcaLznt1ExampleCompressed decompresses to ([MS-XCA]
// section 3.3); sizeof(), not strlen(), is the byte count since the
// terminal NUL is part of the data.
inline constexpr char kXcaLznt1ExampleDecompressed[] =
    "F# F# G A A G F# E D D E F# F# E E F# F# G A A G F# E D D E F# E D D E E "
    "F# D E F# G F# D E F# G F# E D E A F# F# G A A G F# E D D E F# E D D";

inline constexpr size_t kXcaLznt1ExampleDecompressedSize =
    sizeof(kXcaLznt1ExampleDecompressed);
static_assert(kXcaLznt1ExampleDecompressedSize == 142,
              "[MS-XCA] section 3.3's worked example decompresses to exactly "
              "142 bytes, terminal NUL included");

// Hand-encodes an LZNT1 "uncompressed chunk" ([MS-XCA] 2.5.1.2): header plus
// payload verbatim, so fixtures never need an actual compressor.
[[nodiscard]] std::vector<BYTE>
    MakeUncompressedLznt1Chunk(std::span<const BYTE> payload);

// Deterministic byte pattern fixtures fill payloads with; callers recompute
// it to check ReadData() results instead of exporting multi-KB arrays.
[[nodiscard]] std::vector<BYTE> CompressionFixturePattern(size_t size);

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// a FILE_ATTRIBUTE_COMPRESSED file; ReadData() must return decompressed bytes.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithCompressedFile();

// Same as BuildFakeNtfsImageWithCompressedFile(), but the unit is *stored*:
// real runs cover it fully (no sparse pad), so its plain bytes
// (CompressionFixturePattern(kCompressionUnitSize)) must pass through as-is.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithStoredCompressionUnit();

// Same as BuildFakeNtfsImageWithCompressedFile(), but the unit is a pure
// hole (no real cluster); must read back as kCompressionUnitSize zeroes.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithSparseCompressionUnit();

// Payload size needing two compressed clusters, still under one unit.
inline constexpr size_t kFragmentedCompressedPayloadSize = 2000;

// Same as BuildFakeNtfsImageWithCompressedFile(), but the unit's compressed
// bytes live in two non-contiguous real runs plus a sparse pad, so reading
// it must stitch fragments together before decompressing.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithFragmentedCompressedFile();

// Sizes of the fixture's two units: a full stored one, then a short one.
inline constexpr size_t kTrailingPartialUnitStoredSize = kCompressionUnitSize;
inline constexpr size_t kTrailingPartialUnitTailSize = 500;

// Same as BuildFakeNtfsImageWithCompressedFile(), but 6 clusters long: a
// full stored unit plus a shorter trailing one at EOF; must read back
// exactly real_size bytes, no over-read.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithTrailingPartialCompressionUnit();

// Same as BuildFakeNtfsImageWithCompressedFile(), but the unit's real
// cluster holds a malformed LZNT1 chunk; decompression must reject it.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithCorruptCompressedUnit();

// Same as BuildFakeNtfsImageWithCompressedFile(), but last_vcn claims two
// units while the run list only maps the first - the state a decode error
// leaves ParseDataRun() in; unit 1 must fail, not read back as zeroes.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithUnmappedCompressionUnit();

// Same as BuildFakeNtfsImageWithCompressedFile(), but the unit's runs are
// [real][sparse][real] - real clusters after a hole, a layout no per-unit
// encoding can produce; ReadData() must reject it, not silently drop them.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithRealClustersAfterHole();

// Decompressed byte count, short of what an interior unit demands.
inline constexpr size_t kShortDecompressedUnitSize = 100;

// Same as BuildFakeNtfsImageWithCompressedFile(), but two units long and the
// first (interior) unit's LZNT1 stream stops early; ReadData() must reject
// it instead of zero-padding, unlike a legitimately short trailing unit.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithShortDecompressedUnit();

// One byte short of the base header plus the CompressedSize field.
inline constexpr DWORD kCompressedAttrTruncatedTotalSize = 71;

// Same as BuildFakeNtfsImageWithCompressedFile(), with total_size forced to
// kCompressedAttrTruncatedTotalSize: passes the base non-resident size gate
// but not the CompressedSize field comp_unit_size implies must be present.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithCompressedAttrMissingCompressedSize();

// comp_unit_size BuildFakeNtfsImageWithCompUnitSizeOutOfRange() declares: a
// shift so large that 1ULL << comp_unit_size would be undefined behaviour.
inline constexpr WORD kCompUnitSizeOutOfRangeShift = 64;

// Same as BuildFakeNtfsImageWithCompressedFile(), with comp_unit_size set to
// kCompUnitSizeOutOfRangeShift; AttrNonResident<S>'s constructor must reject
// the shift before ever using it.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithCompUnitSizeOutOfRange();

// A well-defined shift (2048 clusters) past the largest buffered unit size.
inline constexpr WORD kOversizedCompUnitSizeShift = 11;

// Same as BuildFakeNtfsImageWithCompressedFile(), with comp_unit_size set to
// kOversizedCompUnitSizeShift, exceeding the constructor's size cap.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithOversizedCompressionUnit();

// Not a multiple of kCompressionUnitClusters.
inline constexpr ULONGLONG kMisalignedCompressedStartVcn = 2;

// Same as BuildFakeNtfsImageWithCompressedFile(), with start_vcn set to
// kMisalignedCompressedStartVcn; units are indexed relative to start_vcn, so
// a misaligned one must be rejected, not decoded against a shifted window.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMisalignedCompressedStartVcn();

// LCN where a non-resident $EFS stream lives, clear of the data streams.
inline constexpr DWORD kFakeEfsStreamLcn = 60;

// One non-resident $DATA stream of an encrypted fake file. The bytes are
// stored as given: encrypting them is the test's job.
struct FakeEncryptedStream
{
  std::wstring name;                // empty: the unnamed stream
  std::vector<FakeDataRun> runs;    // its layout, sparse holes included
  std::vector<BYTE> cluster_bytes;  // laid over the real runs, in order
  ULONGLONG real_size{0};
  bool flagged_encrypted{true};  // the 0x4000 bit of the attribute header
  // The compressed attribute flag (bit 0), alongside flagged_encrypted: a
  // combination real NTFS never produces, but a forged record could.
  bool flagged_compressed{false};
};

// An encrypted file: a root record with the ENCRYPTED std-info flag, these
// $DATA streams, and an $EFS stream.
struct FakeEncryptedFile
{
  std::vector<FakeEncryptedStream> streams;
  // Bytes of the $EFS stream. Empty: the record has none.
  std::vector<BYTE> efs_stream;
  // Real EFS keeps $EFS non-resident, as here by default.
  bool efs_resident{false};
};

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// the file described.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithEncryptedFile(const FakeEncryptedFile& file);

// Names, MFT references of the two entries of the encrypted directory below:
// the first sits in its $INDEX_ROOT, the second (a directory) in its index
// block.
inline constexpr std::array<std::wstring_view, 2> kEncryptedDirectoryNames{
    L"secret.txt", L"vault"};
inline constexpr std::array<ULONGLONG, 2> kEncryptedDirectoryMftRefs{40, 41};

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// a directory that has the ENCRYPTED std-info flag: EFS marks a directory so
// that files created in it are encrypted. Its index is not encrypted.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithEncryptedDirectory();

// Same as BuildFakeNtfsImageWithEncryptedDirectory(), but the directory's
// $INDEX_ALLOCATION is also LZNT1-compressed: compression and the record's
// own encryption flag are independent, since $INDEX_ALLOCATION is never
// itself an EFS-decrypted stream.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithCompressedEncryptedDirectory();

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// the smallest legal non-resident $DATA (base header only, comp_unit_size ==
// 0); confirms the compressed-only CompressedSize field doesn't leak in.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithMinimalNonResidentData();

// Name (and UTF-16 length) of the leaf entry the decompressed unit holds.
inline constexpr wchar_t kCompressedIndexEntryName[] = L"Comp";
inline constexpr BYTE kCompressedIndexEntryNameLength = 4;

// mft reference that same entry declares.
inline constexpr ULONGLONG kCompressedIndexEntryMftRef = 55;

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// a compressed DIRECTORY whose $INDEX_ALLOCATION decompresses to a valid
// index block - reachable through FuzzOnce(), unlike a plain $DATA attribute.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithCompressedIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), with the LZNT1
// bytes replaced by BuildFakeNtfsImageWithCorruptCompressedUnit()'s
// malformed chunk, reachable by the fuzz harness on a real byte stream.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithCorruptCompressedIndexAllocation();

// Names of the four entries BuildFakeNtfsImageWithSurrogatePairNames()
// writes: the first two are leaves of the resident (uncompressed)
// $INDEX_ROOT, the last two leaves of the compressed $INDEX_ALLOCATION
// block. Every one holds code points outside the BMP, so each is stored as
// UTF-16 surrogate pairs. They sit in the order the index collates them,
// by UTF-16 code unit, so a lookup can walk from the root into the block.
// Each is a different kind of name:
//   [0] U+13080 EGYPTIAN HIEROGLYPH D010, from a rare SMP script;
//   [1] U+1F41C ANT, an emoji;
//   [2] a ZWJ family sequence: four emoji joined by three U+200D;
//   [3] U+20BB7 CJK IDEOGRAPH, from the SIP (plane 2), so its high surrogate
//       differs from the other names'.
inline constexpr std::array<std::wstring_view, 4> kSurrogateNames{
    L"\U00013080", L"\U0001F41C",
    L"\U0001F468\u200D\U0001F469\u200D\U0001F467\u200D\U0001F466",
    L"\U00020BB7"};

// Whether each entry above is a directory: each node holds one directory
// and one file.
inline constexpr std::array<bool, 4> kSurrogateNameIsDirectory{false, true,
                                                               true, false};

// MFT references the four entries above point at, in the same order.
inline constexpr std::array<ULONGLONG, 4> kSurrogateNameMftRefs{56, 57, 58, 59};

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// a compressed directory whose entries are kSurrogateNames: the first two
// resident in $INDEX_ROOT, the last two inside the LZNT1-wrapped index block
// $INDEX_ALLOCATION decompresses to. Reachable through FuzzOnce().
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithSurrogatePairNames();

////////////////////////////////////////////////////////////////////////////
// Fuzz-corpus-only compressed $INDEX_ALLOCATION fixtures: same recipe as
// BuildFakeNtfsImageWithCompressedIndexAllocation(), since FuzzOnce() only
// ever ReadData()s through $INDEX_ROOT/$INDEX_ALLOCATION, never plain $DATA.
////////////////////////////////////////////////////////////////////////////

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), with
// comp_unit_size set to kCompUnitSizeOutOfRangeShift instead - rejected
// before the data run is ever parsed.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithCompUnitSizeOutOfRangeIndexAllocation();

// Same as above, with comp_unit_size set to kOversizedCompUnitSizeShift: a
// well-defined shift past the largest unit size buffered for.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithOversizedCompressionUnitIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), with start_vcn
// forced to kMisalignedCompressedStartVcn via FakeNonResidentOverrides.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMisalignedStartVcnIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), with total_size
// forced to kCompressedAttrTruncatedTotalSize - a ParseAttrs()-level
// rejection independent of attribute type.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMissingCompressedSizeIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), but unit 0 is
// only partially mapped (last_vcn claims more clusters than the run list
// covers); LeadingRealClusters() must reject it, not treat it as smaller.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithUnmappedCompressionUnitIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), but unit 0's
// runs are [real][hole][real] - a layout no per-unit encoding produces,
// which must be rejected before decompression.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithRealClustersAfterHoleIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), but unit 0 is a
// pure hole (record also marked SPARSE); reads back as zeroes, exercising
// GetCompressionUnit()'s "0 real clusters" branch.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithSparseCompressionUnitIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), but unit 0's
// real cluster decompresses to only kShortDecompressedUnitSize bytes, far
// short of what real_size demands; must be rejected, not zero-padded.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithShortDecompressedUnitIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), but unit 0's
// one real run is at an LCN whose product with cluster_size overflows a
// LONGLONG; the "stored" branch must report a read failure, not throw.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithStoredCompressionUnitBadLcn();

// Same as the "stored" variant above, but only 1 of 4 clusters is real (same
// overflowing LCN); exercises the "compressed" branch's read failure instead.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithCompressedCompressionUnitBadLcn();

////////////////////////////////////////////////////////////////////////////
// LZNT1 decompressor rejection-path corpus fixtures (src/lznt1/decompress.cpp):
// each shaped like BuildFakeNtfsImageWithCorruptCompressedIndexAllocation(),
// with different bytes targeting a different bound inside Decompress().
////////////////////////////////////////////////////////////////////////////

// Chunk header 0x1002: bits 14-12 != the mandatory signature 3; Decompress()
// must reject this before looking at anything else in the chunk.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1InvalidSignatureIndexAllocation();

// Chunk header 0xBFFF declares a 4096-byte payload, far more than the bytes
// actually available in this fixture's one real cluster.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1ChunkExceedsSrcBoundsIndexAllocation();

// Two chunks: one decompressing to 4088 bytes, then an uncompressed chunk
// declaring 10 more - more than the 4096-byte unit buffer has left.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1UncompressedChunkExceedsDestIndexAllocation();

// One chunk decompressing to exactly kChunkSize (4096) bytes, with one more
// data element left undecoded; the "already produced a full chunk" bound
// must fire before that next element is even looked at.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1ChunkOver4096IndexAllocation();

// Chunk 1 fills the dest buffer to exactly 4096 bytes and ends cleanly;
// chunk 2's first element is a literal byte, which must be rejected the
// moment out == dest.size(), without reading the literal's own value.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1LiteralExceedsDestIndexAllocation();

// One chunk whose only element is a compressed word, but only 1 byte of
// payload remains after the flag byte, not the 2 bytes a word needs.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1TruncatedWordIndexAllocation();

// Uses comp_unit_size == 1 (2048-byte unit, smaller than every other
// fixture's), so a back-reference within the 4096-byte per-chunk cap can
// still be rejected purely for exceeding this smaller unit's own buffer.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1BackreferenceExceedsDestIndexAllocation();

}  // namespace NtfsBrowserTests
