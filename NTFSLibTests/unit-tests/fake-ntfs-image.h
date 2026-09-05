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

// Real on-disk size (bytes) of the $VOLUME_INFORMATION attribute: an 8-byte
// reserved field, 1-byte major version, 1-byte minor version and a 2-byte
// flags field - 12 bytes total, with no trailing padding on disk. Hardcoded
// here, independent of sizeof(Attr::VolumeInformation), so this fixture keeps
// exercising the true on-disk size even if that struct's own layout changes
// again - regression fixture for the bug fixed alongside F44
// (docs/bug-reports/2026-09-03-full-repo.md): AttrVolInfo's ctor rejects any
// attribute smaller than sizeof(Attr::VolumeInformation), but without
// #pragma pack(1) that sizeof() is inflated to 16 by alignof(ULONGLONG)
// padding, so it rejected every real volume's (12-byte) attribute. The
// default BuildFakeNtfsImage() fixture couldn't catch this on its own: it
// sizes its $Volume record off the very same (possibly wrong) sizeof(), so it
// stays self-consistent with the bug instead of exposing it.
inline constexpr WORD kMinimalVolumeInformationSize = 12;

// Same volume as BuildFakeNtfsImage(), with $Volume's (#3) VOLUME_INFORMATION
// attribute shrunk to exactly kMinimalVolumeInformationSize (12) bytes - the
// true on-disk size - instead of whatever sizeof(Attr::VolumeInformation)
// currently computes to. Must still open successfully (IsVolumeOK() ==
// true) and report version 3.1.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMinimalVolumeInformation();

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

// clusters_per_index_block value BuildFakeNtfsImageWithOversizedIndexBlock()
// patches into the BPB - regression fixture for F5:
// NtfsVolume<S>::ParseBootSector() (src/ntfs-volume.cpp) reads
// clusters_per_index_block as a signed char (sz); for sz <= 0 it computes
// index_block_size_ = 1U << static_cast<unsigned char>(-sz). 0xE1 -> sz =
// -31, a shift amount that is still < 32 (so, unlike sz = -128, this does
// NOT itself invoke undefined behaviour on the 32-bit 1U), but the result -
// 1U << 31 == 0x80000000 (2 GiB) - is nowhere near a plausible index block
// size, yet F6's own bound check (index_block_size_ >= sizeof(Data::
// IndexBlock) && index_block_size_ % sector_size_ == 0) does not catch it:
// 0x80000000 is far bigger than 40 and evenly divisible by every common
// sector size. See F5 in docs/bug-reports/2026-09-03-full-repo.md.
inline constexpr BYTE kOversizedClustersPerIndexBlock = 0xE1;

// index_block_size_ that kOversizedClustersPerIndexBlock is expected to
// produce.
inline constexpr DWORD kOversizedIndexBlockSize = 0x80000000;

// Same volume as BuildFakeNtfsImage(), with clusters_per_index_block patched
// to kOversizedClustersPerIndexBlock so GetIndexBlockSize() ends up
// kOversizedIndexBlockSize (2 GiB) - a well-defined (no UB in the shift
// itself) but absurd size that F6's existing checks do not reject. See F5 in
// docs/bug-reports/2026-09-03-full-repo.md.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithOversizedIndexBlock();

// clusters_per_file_record value BuildFakeNtfsImageWithOversizedFileRecord()
// patches into the BPB - same reasoning and same 0xE1 -> sz = -31 -> 1U << 31
// == 0x80000000 shift as kOversizedClustersPerIndexBlock above, but for
// file_record_size_ instead of index_block_size_. See F5 in
// docs/bug-reports/2026-09-03-full-repo.md.
inline constexpr BYTE kOversizedClustersPerFileRecord = 0xE1;

// file_record_size_ that kOversizedClustersPerFileRecord is expected to
// produce.
inline constexpr DWORD kOversizedFileRecordSize = 0x80000000;

// Same volume as BuildFakeNtfsImage(), with clusters_per_file_record patched
// to kOversizedClustersPerFileRecord so GetFileRecordSize() ends up
// kOversizedFileRecordSize (2 GiB). Unlike the index-block variant above,
// this field is actually consumed by FileRecord<S>::ReadFileRecord() the
// moment any file record is read (record_buffer_.resize(GetFileRecordSize())),
// which NtfsVolume<S>::Init() does unconditionally for the $Volume record
// before ever reporting IsVolumeOK() - so this fixture is deliberately not
// driven through a full NtfsVolume construction in tests (that would itself
// perform the very unbounded 2 GiB allocation F5 is about preventing).
// See F5 in docs/bug-reports/2026-09-03-full-repo.md.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithOversizedFileRecord();

// clusters_per_file_record value
// BuildFakeNtfsImageWithFileRecordSizeTooBig() patches into the BPB -
// regression fixture for F9: NtfsVolume<S>::ParseBootSector()
// (src/ntfs-volume.cpp) reads clusters_per_file_record as a signed char
// (sz); for sz > 0 it computes file_record_size_ = cluster_size_ * sz. sz ==
// 8 is the largest magnitude F5's own bound on sz still allows ([-12, 8]),
// and with this fixture's 1024-byte cluster_size_ yields file_record_size_
// == kFileRecordSizeTooBig (8192) - a whole number of sectors, comfortably
// >= kMinFileRecordHeaderSize, so every check that predates F9 accepts it,
// yet it is twice FileRecordHeader::kMaxFileRecordSize (4096), the largest
// size FileRecordHeader::Data::raw can actually hold. See F9 in
// docs/bug-reports/2026-09-03-full-repo.md.
inline constexpr BYTE kFileRecordSizeTooBigClustersPerFileRecord = 8;

// file_record_size_ that kFileRecordSizeTooBigClustersPerFileRecord is
// expected to produce (cluster_size_ * sz == 1024 * 8).
inline constexpr DWORD kFileRecordSizeTooBig = 8192;

// Same volume as BuildFakeNtfsImage(), with clusters_per_file_record patched
// to kFileRecordSizeTooBigClustersPerFileRecord so GetFileRecordSize() would
// end up kFileRecordSizeTooBig (8192) - legal under every check that
// predates F9 (a whole number of sectors, an sz magnitude within F5's own
// bound) but exceeding FileRecordHeader::kMaxFileRecordSize. Unlike
// BuildFakeNtfsImageWithOversizedFileRecord() above, this fixture's whole
// point is that NtfsVolume<S>::ParseBootSector() must now reject it outright
// - it is used only as boot-sector-only bytes (see
// NTFSLibTests/fuzz/data/file_record_size_too_big), never driven through a
// full NtfsVolume construction. See F9 in
// docs/bug-reports/2026-09-03-full-repo.md.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithFileRecordSizeTooBig();

// lcn_mft value BuildFakeNtfsImageWithHugeMftLcn() patches into the BPB -
// regression fixture for F8: NtfsVolume<S>::ParseBootSector() computes
// mft_addr_ = bpb->lcn_mft * cluster_size_ (src/ntfs-volume.cpp) without ever
// validating the result against the volume's own size. With cluster_size_ ==
// kClusterSize (1024, this fixture's fixed cluster size), 2^53 yields
// mft_addr_ == 2^63 exactly: it still fits in the ULONGLONG mft_addr_ is
// stored as, but FileRecord<S>::ReadFileRecord() (src/file-record.cpp)
// immediately narrows GetMFTAddr() (plus a per-record offset) down to a
// LONGLONG for LARGE_INTEGER::QuadPart via gsl::narrow<LONGLONG>() - a value
// that large makes that narrow() throw gsl::narrowing_error. That type
// derives from std::exception, not std::runtime_error, so it escapes every
// catch (const std::runtime_error&) in the parser and propagates out of
// FileRecord::ParseFileRecord() -> NtfsVolume::Init() -> the NtfsVolume
// constructor itself. See F8 in docs/bug-reports/2026-09-03-full-repo.md.
inline constexpr ULONGLONG kHugeMftLcn = 1ULL << 53;

// Same volume as BuildFakeNtfsImage(), with lcn_mft patched to kHugeMftLcn so
// mft_addr_ ends up 2^63 - large enough that gsl::narrow<LONGLONG>() throws
// when FileRecord<S>::ReadFileRecord() converts it (plus a per-record
// offset) to a LARGE_INTEGER, yet NtfsVolume<S>::ParseBootSector() currently
// accepts the volume regardless, since nothing validates mft_addr_ against
// the volume's own bounds. See F8 in docs/bug-reports/2026-09-03-full-repo.md.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithHugeMftLcn();

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

// Same volume as BuildFakeNtfsImage(), but with the $MFT (#0) file record
// zero-filled (magic == 0, not kFileRecordMagic) so
// FileRecord<S>::ParseFileRecord() fails on it, while $Volume (#3) and the
// root directory (#5) records are left untouched and valid - regression
// fixture for F14: NtfsVolume<S>::Init() (src/ntfs-volume.cpp) sets
// volume_ok_ = true right after $Volume is successfully parsed, *before*
// mft_record_.ParseFileRecord()/ParseAttrs() run and mft_data_ is assigned.
// If either of those fails - as it does here - Init() returns early with
// mft_data_ still nullptr but volume_ok_ already true, breaking the
// documented "construct, then check IsVolumeOK()" contract:
// GetRecordsCount() (src/ntfs-volume.cpp) dereferences mft_data_
// unconditionally and is marked noexcept. See F14 in
// docs/bug-reports/2026-09-03-full-repo.md.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithCorruptMftRecord();

// MFT index of the record built by
// BuildFakeNtfsImageWithAttrNameExceedsTotalSize(): its only attribute (a
// resident, named $DATA) declares name_offset/name_length that read past its
// own total_size. Free below Enum::MftIdx::USER (16) - see
// kUafExtensionIdx0/1's comment above for why only this and a couple of
// other small indices remain unclaimed.
inline constexpr ULONGLONG kAttrNameExceedsTotalSizeRecordIdx = 4;

// name_offset/name_length (relative to the attribute's own start, per
// AttrHeaderCommon) BuildFakeNtfsImageWithAttrNameExceedsTotalSize() forges
// for its single $DATA attribute - regression fixture for F2:
// AttrBase<S>::GetAttrName() (src/attr-base.cpp) builds a std::wstring_view
// straight from name_offset/name_length without ever checking
// name_offset + 2*name_length against total_size (or the record's own
// end). kAttrNameBoundsNameOffset (100) + 2*kAttrNameBoundsNameLength (6) =
// 112 comfortably exceeds the attribute's declared total_size (28: a bare
// 24-byte Attr::HeaderResident header plus a 4-byte body), while still
// landing well inside the same 1024-byte record buffer - see
// kAttrNameBoundsSentinel below for the deterministic bytes it reads
// instead of a real name. See F2 in docs/bug-reports/2026-09-03-full-repo.md.
inline constexpr WORD kAttrNameBoundsNameOffset = 100;
inline constexpr BYTE kAttrNameBoundsNameLength = 6;

// Deterministic UTF-16 bytes BuildFakeNtfsImageWithAttrNameExceedsTotalSize()
// writes at kAttrOffset + kAttrNameBoundsNameOffset - i.e. past the
// attribute's declared total_size, but still well inside the 1024-byte
// record buffer, so the pre-fix wrong result (GetAttrName() returning this
// exact string) is deterministic rather than depending on adjacent heap
// contents. Exactly kAttrNameBoundsNameLength wide characters (excluding the
// terminating NUL).
inline constexpr wchar_t kAttrNameBoundsSentinel[] = L"PWNED!";

// Same volume as BuildFakeNtfsImage(), plus a record
// (kAttrNameExceedsTotalSizeRecordIdx) whose single resident $DATA attribute
// declares name_offset/name_length (kAttrNameBoundsNameOffset/
// kAttrNameBoundsNameLength) reaching past its own total_size while still
// landing on known, deterministic bytes (kAttrNameBoundsSentinel) inside the
// same 1024-byte record buffer - regression fixture for F2. See
// kAttrNameExceedsTotalSizeRecordIdx above for the full defect description.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithAttrNameExceedsTotalSize();

// offset_of_attr value BuildFakeNtfsImageWithAttrOffsetOutOfBounds() patches
// into the root directory record (#5) - regression fixture for the
// HeaderCommon() half of bug F9: FileRecordHeader::HeaderCommon()
// (src/data/file-record-header.cpp) bounds offset_of_attr against this
// instance's own buffer_size_ (kFakeFileRecordSize, 1024, for every record in
// this fixture), so a value this far past it - well within a WORD's range -
// must be rejected instead of read past the record. See F9 in
// docs/bug-reports/2026-09-03-full-repo.md.
inline constexpr WORD kAttrOffsetOutOfBounds = 2000;

// Same volume as BuildFakeNtfsImage(), with the root directory's (#5)
// offset_of_attr patched to kAttrOffsetOutOfBounds - regression fixture for
// FileRecordHeader::HeaderCommon()'s buffer_size_ bound (bug F9):
// FileRecord<S>::ParseAttrs() calls HeaderCommon() to locate the first
// attribute, so this must make ParseAttrs() reject the whole record instead
// of reading past its real, exactly-1024-byte extent.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttrOffsetOutOfBounds();

// Known, recognizable byte pattern BuildFakeNtfsImageWithSmallResidentData()
// writes as the entire body of its single resident $DATA attribute -
// regression fixture for F10: AttrResident<S>::ReadData() computed the real,
// possibly-truncated byte count actually memcpy'd ("actural") but returned
// the full requested buffer length ("bufLen") instead. A caller whose buffer
// is larger than this attribute's real data size (4 bytes) can then tell the
// two apart: the returned length either matches this array's size (fixed) or
// the buffer's own, larger size (buggy).
inline constexpr std::array<BYTE, 4> kSmallResidentDataContent{0xDE, 0xAD, 0xBE,
                                                               0xEF};

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by a bare file record whose only attribute is a resident $DATA
// attribute holding exactly kSmallResidentDataContent - regression fixture
// for F10 (docs/bug-reports/2026-09-03-full-repo.md): a caller reading with a
// buffer bigger than this attribute's real size (as
// NTFSLibTests/ntfsundel/ntfsundelDlg.cpp does, always requesting 64KiB) must
// see ReadData() report the true, possibly-truncated byte count, not the
// buffer's requested length. Following the precedent set by
// BuildFakeNtfsImageWithAttrOffsetOutOfBounds() above, this overwrites the
// root record (#5) directly in its own independent image copy rather than
// claiming a new MFT index - every index below Enum::MftIdx::USER (16) is
// already claimed by another fixture in this file.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithSmallResidentData();

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by a bare file record whose only attribute is a resident
// $ATTRIBUTE_LIST 50 bytes long - not an exact multiple of
// sizeof(Attr::AttributeList) (40) - regression fixture for the
// AttrList<S>::AttrList() hardening added alongside F10
// (docs/bug-reports/2026-09-03-full-repo.md, src/attr-list.cpp): al_record
// is a single stack variable reused across loop iterations, never reset
// between them, so a short final read must stop the loop cleanly (now
// observably, via an NTFS_TRACE2 call) instead of parsing a "phantom" final
// entry built from a mix of freshly-read and stale bytes. The one full entry
// this fixture does declare names itself (base_ref.segment_number == this
// record's own file reference, 5) so AttrList's ctor never needs to resolve
// an extension record at all, keeping the fixture self-contained. Same
// "overwrite the root record in its own image copy" technique as
// BuildFakeNtfsImageWithSmallResidentData() above.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttributeListShortRead();

// MFT index of the second record BuildFakeNtfsImageWithAttributeListCycle()
// builds - the root directory's (#5) own resident $ATTRIBUTE_LIST points
// here, and this record's own resident $ATTRIBUTE_LIST points right back at
// #5, forming a two-record resolution cycle.
inline constexpr ULONGLONG kAttrListCycleExtIdx = 6;

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// and a second record (kAttrListCycleExtIdx) each replaced by a bare file
// record whose sole attribute is a resident $ATTRIBUTE_LIST with a single
// entry of attr_type ATTRIBUTE_LIST naming the OTHER of the two records -
// regression fixture for AttrList<S>::AttrList()'s cycle guard
// (src/attr-list.cpp): resolving #5's $ATTRIBUTE_LIST entry recurses into
// kAttrListCycleExtIdx's own ParseAttrs(), whose $ATTRIBUTE_LIST entry
// names #5 again for the very same attribute type - a key already inserted
// into attrListChain before #5's own AttrList ctor ever started resolving
// entries, so this must be recognized and skipped ("...already resolved in
// this chain, skipping") instead of recursing without bound. Also
// regenerates the fuzz corpus entry
// NTFSLibTests/fuzz/data/attribute_list_extension_record_cycle in a
// deterministic, from-scratch form (see fuzzer-regression-tests.cpp): the
// original raw AFL find no longer reaches this message unmodified once the
// F10 fix (docs/bug-reports/2026-09-03-full-repo.md) changed
// AttrResident<S>::ReadData()'s return value for every resident
// $ATTRIBUTE_LIST read, this fixture's single, exactly-40-byte entry on
// each record sidesteps that entirely.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttributeListCycle();

// Real, fixed on-disk size (bytes) of a nameless $ATTRIBUTE_LIST entry's
// header: attr_type(4) + record_size(2) + name_length(1) + name_offset(1) +
// start_vcn(8) + base file reference(8) + attr_id(2) = 26 bytes, with no
// trailing padding on disk. Hardcoded here, independent of
// sizeof(Attr::AttributeList) - regression fixture for bug F11
// (docs/bug-reports/2026-09-03-full-repo.md): Attr::MftSegmentReference's
// non-bitfield WORD member inflates that sizeof() to 40 (and still 32 even
// once fixed, via alignof(ULONGLONG) padding with no on-disk counterpart) -
// so a fixture sizing its entries off that sizeof() instead stays
// self-consistent with the bug rather than exposing it, the same trap
// kMinimalVolumeInformationSize above documents for $VOLUME_INFORMATION.
inline constexpr WORD kAttributeListRealEntrySize = 26;

// MFT index of the directory record built by
// BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory(): its resident
// $ATTRIBUTE_LIST holds TWO entries, packed back-to-back with NO padding
// (each exactly kAttributeListRealEntrySize bytes) - relocating
// $INDEX_ROOT to kAttrListTightPackExtIdxA and $INDEX_ALLOCATION to
// kAttrListTightPackExtIdxB.
inline constexpr ULONGLONG kAttrListTightPackDirIdx = 6;

// MFT index of the extension record kAttrListTightPackDirIdx's
// $ATTRIBUTE_LIST relocates $INDEX_ROOT to - the same single "Foo" entry
// (file reference 20) as MakeIndexRootExtensionRecord() always builds.
inline constexpr ULONGLONG kAttrListTightPackExtIdxA = 7;

// MFT index of the extension record kAttrListTightPackDirIdx's
// $ATTRIBUTE_LIST relocates $INDEX_ALLOCATION to - a minimal non-resident
// $INDEX_ALLOCATION whose real_size is kAttrListTightPackRealSize.
inline constexpr ULONGLONG kAttrListTightPackExtIdxB = 8;

// real_size BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory()
// writes into kAttrListTightPackExtIdxB's $INDEX_ALLOCATION attribute - read
// back via AttrBase::GetDataSize() to confirm that attribute was genuinely
// resolved (as opposed to merely not crashing).
inline constexpr DWORD kAttrListTightPackRealSize = 4096;

// Same volume as BuildFakeNtfsImage(), plus a directory
// (kAttrListTightPackDirIdx) whose resident $ATTRIBUTE_LIST is exactly
// 2 * kAttributeListRealEntrySize (52) bytes - a tight multiple of the REAL
// 26-byte on-disk entry size, but NOT a multiple of
// sizeof(Attr::AttributeList) (40 pre-F11-fix) - regression fixture for bug
// F11 (docs/bug-reports/2026-09-03-full-repo.md): AttrList<S>::AttrList()'s
// read loop (src/attr-list.cpp) requests sizeof(Attr::AttributeList) bytes
// per iteration; with only 26 real bytes per on-disk entry, that first
// (40-byte) read already reads past entry 1 into entry 2's own leading
// bytes, and the second iteration's request (offset 26, only 26 real bytes
// left) comes up short, so entry 2 ($INDEX_ALLOCATION) is silently dropped -
// it never resolves, even though the volume is perfectly well-formed. Entry
// 1 ($INDEX_ROOT) still happens to resolve, since every field the loop reads
// before the buggy base_ref/attr_id region lands on real bytes regardless of
// the struct bug.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory();

}  // namespace NtfsBrowserTests
