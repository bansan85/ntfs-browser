#pragma once

#include <array>
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

// MFT index of the directory record built by
// BuildFakeNtfsImageWithMultiTypeAttributeListDirectory(): its resident
// $ATTRIBUTE_LIST holds TWO records that both name the SAME extension
// record (kMultiTypeExtensionIdx) but for different attribute types
// ($INDEX_ROOT then $INDEX_ALLOCATION).
inline constexpr ULONGLONG kAttrListMultiTypeDirIdx = 10;

// MFT index of the extension record kAttrListMultiTypeDirIdx's
// $ATTRIBUTE_LIST points at for both entries. Holds both a resident
// $INDEX_ROOT (the same single "Foo" entry, file reference 20, as
// MakeIndexRootExtensionRecord()) and a minimal non-resident
// $INDEX_ALLOCATION (empty data run, real_size 0 - just enough for
// FileRecord::ParseAttr() to construct an AttrIndexAlloc).
inline constexpr ULONGLONG kMultiTypeExtensionIdx = 11;

// Same volume as BuildFakeNtfsImage(), plus a directory
// (kAttrListMultiTypeDirIdx) whose $ATTRIBUTE_LIST relocates two different
// attribute types ($INDEX_ROOT and $INDEX_ALLOCATION) into the SAME
// extension record (kMultiTypeExtensionIdx) - regression fixture for F17's
// primary defect: AttrList<S>::AttrList() (src/attr-list.cpp) deduplicates
// fr.attr_list_chain_ on record_ref alone, so once the first
// $ATTRIBUTE_LIST entry resolves kMultiTypeExtensionIdx, the second entry
// naming the very same record but a DIFFERENT attr_type is skipped even
// though its attribute was never actually retrieved. Real directories (eg.
// C:\Windows) commonly relocate $INDEX_ROOT and $INDEX_ALLOCATION into the
// same extension record this way. See F17 in
// docs/bug-reports/2026-09-03-full-repo.md.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMultiTypeAttributeListDirectory();

// MFT index of a second directory record built by
// BuildFakeNtfsImageWithAttributeListDirectoryChainReused(): its
// $ATTRIBUTE_LIST also relocates $INDEX_ROOT to kIndexExtensionIdx - the
// very same extension record kAttributeListDirIdx's own $ATTRIBUTE_LIST
// resolves.
inline constexpr ULONGLONG kAttributeListDirIdx2 = 12;

// Same volume as BuildFakeNtfsImageWithAttributeListDirectory(), plus a
// second, independent directory record (kAttributeListDirIdx2) whose
// $ATTRIBUTE_LIST also relocates $INDEX_ROOT to kIndexExtensionIdx -
// regression fixture for F17's secondary defect: FileRecord::attr_list_chain_
// (include/ntfs-browser/file-record.h) is never reset by
// FileRecord::ParseFileRecord() (src/file-record.cpp), so a FileRecord
// object reused across two ParseFileRecord()/ParseAttrs() calls (as
// ntfsdir.exe and ntfsundel.exe do) carries the first parse's resolved
// record set into the second: kIndexExtensionIdx, already marked resolved
// by the first directory's chain, is then wrongly treated as
// already-resolved for the second, unrelated directory's own chain and
// skipped. See F17 in docs/bug-reports/2026-09-03-full-repo.md.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithAttributeListDirectoryChainReused();

// MFT index of the directory record built by
// BuildFakeNtfsImageWithFragmentedAttributeListDirectory(): its resident
// $ATTRIBUTE_LIST relocates $INDEX_ALLOCATION across FOUR DISTINCT
// extension records (kUafExtensionIdx0..3) - as opposed to
// kAttrListMultiTypeDirIdx above, which relocates two different types into
// the SAME record. Regression fixture for N3
// (docs/bug-reports/2026-09-03-full-repo.md): AttrList<S>::AttrList()
// (src/attr-list.cpp) grows file_record_list_ - a
// std::vector<FileRecord<S>> - by one emplace_back() per resolved
// extension record; in Strategy::FULL_CACHE each FileRecord's raw record
// buffer is a by-value member of the FileRecord itself, so once a later
// emplace_back() reallocates the vector, every FileRecord already
// constructed - and every attribute already merged out of it - is moved in
// memory, leaving AttrBase::attr_header_/AttrNonResident::attr_header_nr_
// (references bound at construction time) dangling. Four distinct records
// guarantee at least one reallocation regardless of std::vector's growth
// factor (eg. MSVC's ~1.5x would already reallocate by the 2nd-4th
// emplace_back).
inline constexpr ULONGLONG kUafAttrListDirIdx = 13;

// MFT indices of the four extension records
// BuildFakeNtfsImageWithFragmentedAttributeListDirectory()'s
// $ATTRIBUTE_LIST relocates $INDEX_ALLOCATION to, one entry/record each.
// Each holds a minimal non-resident $INDEX_ALLOCATION whose real_size is a
// distinct, recognizable sentinel (kUafRealSizeSentinels) so a test can
// tell whether it is still reading that specific record's own bytes, or a
// dangling/relocated one, after the whole chain is resolved.
//
// All four (and kUafAttrListDirIdx above) must stay below
// Enum::MftIdx::USER (16, include/ntfs-browser/mft-idx.h):
// FileRecord::ReadFileRecord() (src/file-record.cpp) only takes the simple
// "direct disk allocation" path this fixture relies on (mftAddr +
// fileRecordSize * fileRef) for fileRef < 16; at or above that, it instead
// reads through volume_.mft_data_ (the fake $MFT's own, deliberately empty,
// DATA attribute), which this minimal fixture does not populate. Since only
// 13, 14 and 15 remain free below that threshold after the other fixtures'
// indices above, kUafExtensionIdx2/3 reuse 1 and 2 - reserved for
// $MFTMirr/$LogFile in a real volume, but never written to by
// BuildFakeNtfsImage() and so harmlessly zero-filled/unused here.
inline constexpr ULONGLONG kUafExtensionIdx0 = 14;
inline constexpr ULONGLONG kUafExtensionIdx1 = 15;
inline constexpr ULONGLONG kUafExtensionIdx2 = 1;
inline constexpr ULONGLONG kUafExtensionIdx3 = 2;

// real_size sentinel values BuildFakeNtfsImageWithFragmentedAttributeListDirectory()
// writes into kUafExtensionIdx0..3's $INDEX_ALLOCATION, in order - each a
// distinct multiple of the fake image's 1024-byte cluster/index-block size,
// read back via AttrBase::GetDataSize() (AttrNonResident<S>::GetDataSize(),
// which reads attr_header_nr_.real_size fresh on every call - exactly the
// dangling-reference read N3 describes).
inline constexpr std::array<DWORD, 4> kUafRealSizeSentinels{1024, 2048, 3072,
                                                            4096};

// Same volume as BuildFakeNtfsImage(), plus a directory
// (kUafAttrListDirIdx) whose $ATTRIBUTE_LIST relocates $INDEX_ALLOCATION
// into four distinct extension records (kUafExtensionIdx0..3) - regression
// fixture for N3. See kUafAttrListDirIdx above for the full defect
// description.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithFragmentedAttributeListDirectory();

}  // namespace NtfsBrowserTests
