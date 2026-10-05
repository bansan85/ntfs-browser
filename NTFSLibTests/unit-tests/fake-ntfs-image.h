#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gap-collation-probe.h"
#include "named-stream-probe.h"

namespace NtfsBrowserTests
{

// Fake record count $MFT reports; arbitrary, tests just check it survives.
inline constexpr uint64_t sentinel_record_count = 5;

// Every fake record's size; FileRecordHeader asserts on this size internally.
inline constexpr uint32_t fake_file_record_size = 1024;

// Volume geometry every image built here declares in its BPB: 512-byte
// sectors, two per file record and per cluster.
inline constexpr WORD fake_bytes_per_sector = 512;
inline constexpr BYTE fake_sectors_per_cluster = 2;
// Use this, not fake_file_record_size, for anything sized in clusters.
inline constexpr DWORD fake_cluster_size =
    static_cast<DWORD>(fake_bytes_per_sector) * fake_sectors_per_cluster;

// Builds a minimal fake NTFS volume image in memory: boot sector, $MFT,
// $Volume, and root directory records, just enough for NtfsVolume<S> to
// open it.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImage();

// Writes BuildFakeNtfsImage()'s image to a temp file and returns its path.
[[nodiscard]] std::filesystem::path WriteFakeNtfsImage();

// True on-disk size of $VOLUME_INFORMATION: 12 bytes, with no padding.
inline constexpr WORD minimal_volume_information_size = 12;

// Same volume as BuildFakeNtfsImage(), with $Volume's (#3) VOLUME_INFORMATION
// attribute shrunk to minimal_volume_information_size bytes.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMinimalVolumeInformation();

// Same volume as BuildFakeNtfsImage(), with $Volume's (#3) VOLUME_INFORMATION
// attribute declaring no body at all (attr_size 0).
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithEmptyVolumeInformation();

// Volume name BuildFakeNtfsImageWithVolumeName() stores in $Volume's
// VOLUME_NAME attribute. ASCII, so its UTF-8 form is the same bytes.
inline constexpr std::wstring_view fake_volume_name = L"TESTVOL";

// Same volume as BuildFakeNtfsImage(), plus a VOLUME_NAME attribute
// holding fake_volume_name on $Volume (#3).
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithVolumeName();

// Directory record index: only attribute is a resident $ATTRIBUTE_LIST
// relocating $INDEX_ROOT to index_extension_idx.
inline constexpr ULONGLONG attribute_list_dir_idx = 6;

// Extension record index: holds the $INDEX_ROOT attribute_list_dir_idx's
// $ATTRIBUTE_LIST points to, with a single named entry "Foo" (ref 20).
inline constexpr ULONGLONG index_extension_idx = 7;

// Same volume as BuildFakeNtfsImage(), plus a directory split across two
// records the way real NTFS directories (eg. C:\Windows) can be: the base
// record holds only $ATTRIBUTE_LIST, and $INDEX_ROOT lives in the
// extension record it points to.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttributeListDirectory();

// MFT index of the record built by
// BuildFakeNtfsImageWithUndersizedAttribute(): its only attribute declares
// total_size = 17, smaller than sizeof(Attr::HeaderResident) (24).
inline constexpr ULONGLONG undersized_attr_record_idx = 8;

// Same volume as BuildFakeNtfsImage(), plus a record (undersized_attr_record_idx)
// whose single attribute is smaller than a resident attribute's own fixed
// header - regression fixture for an attribute whose size/offset fields get
// read from bytes past its own declared extent.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithUndersizedAttribute();

// MFT index of the directory record built by
// BuildFakeNtfsImageWithForgedIndexBlock(); the next unused index.
inline constexpr ULONGLONG index_alloc_dir_idx = 9;

// Size (bytes) of the forged index block: a whole number of clusters,
// kept distinct from every other allocation size in this file.
inline constexpr DWORD forged_index_block_size = 7 * fake_file_record_size;

// offset_of_us declared by the forged index block: far past
// forged_index_block_size, overrunning the block's buffer.
inline constexpr WORD forged_index_block_offset_of_us = 0xFFFF;

// Same volume as BuildFakeNtfsImage(), plus a directory whose
// $INDEX_ALLOCATION points at an index block with an out-of-bounds
// offset_of_us.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithForgedIndexBlock();

// clusters_per_index_block patched in by
// BuildFakeNtfsImageWithTinyIndexBlock(); read as a signed char, 0xFF
// becomes -1, producing a far too small index_block_size_.
inline constexpr BYTE tiny_clusters_per_index_block = 0xFF;

// index_block_size_ that tiny_clusters_per_index_block is expected to produce.
inline constexpr DWORD tiny_index_block_size = 2;

// Same volume as BuildFakeNtfsImage(), with clusters_per_index_block patched
// so GetIndexBlockSize() is far smaller than Data::IndexBlock's own header.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithTinyIndexBlock();

// clusters_per_index_block patched in by
// BuildFakeNtfsImageWithOversizedIndexBlock(); read as a signed char, 0xE1
// becomes -31, producing an index_block_size_ far larger than any real
// volume's, yet still divisible by every common sector size.
inline constexpr BYTE oversized_clusters_per_index_block = 0xE1;

// index_block_size_ that oversized_clusters_per_index_block is expected to
// produce.
inline constexpr DWORD oversized_index_block_size = 0x80000000;

// Same volume as BuildFakeNtfsImage(), with clusters_per_index_block patched
// so GetIndexBlockSize() is far larger than any real volume's, while still
// passing every existing bound check.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithOversizedIndexBlock();

// clusters_per_file_record patched in by
// BuildFakeNtfsImageWithOversizedFileRecord(); same 0xE1 shift as
// oversized_clusters_per_index_block, but for file_record_size_.
inline constexpr BYTE oversized_clusters_per_file_record = 0xE1;

// file_record_size_ that oversized_clusters_per_file_record is expected to
// produce.
inline constexpr DWORD oversized_file_record_size = 0x80000000;

// Same volume as BuildFakeNtfsImage(), with clusters_per_file_record patched
// so GetFileRecordSize() is far larger than any real volume's. Unlike the
// index-block variant above, this size is read the moment any file record
// is parsed, so this fixture is not driven through a full NtfsVolume
// construction in tests.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithOversizedFileRecord();

// Yields a file_record_size_ twice FileRecordHeader::max_file_record_size.
inline constexpr BYTE file_record_size_too_big_clusters_per_file_record = 8;

// file_record_size_ that file_record_size_too_big_clusters_per_file_record produces.
inline constexpr DWORD file_record_size_too_big = 8192;

// Same volume as BuildFakeNtfsImage(), with clusters_per_file_record patched
// so GetFileRecordSize() would exceed FileRecordHeader::max_file_record_size.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithFileRecordSizeTooBig();

// lcn_mft patched in by BuildFakeNtfsImageWithHugeMftLcn(); with this
// fixture's fixed cluster size, mft_addr_ ends up exactly 2^63.
inline constexpr ULONGLONG huge_mft_lcn = 1ULL << 53U;

// Same volume as BuildFakeNtfsImage(), with lcn_mft patched to huge_mft_lcn
// so mft_addr_ ends up too large for a LONGLONG.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithHugeMftLcn();

// MFT index of the directory record built by
// BuildFakeNtfsImageWithMultiTypeAttributeListDirectory().
inline constexpr ULONGLONG attr_list_multi_type_dir_idx = 10;

// MFT index of the extension record attr_list_multi_type_dir_idx's
// $ATTRIBUTE_LIST points at for both entries.
inline constexpr ULONGLONG multi_type_extension_idx = 11;

// Same volume as BuildFakeNtfsImage(), plus a directory whose
// $ATTRIBUTE_LIST relocates two different attribute types ($INDEX_ROOT
// and $INDEX_ALLOCATION) into the same extension record.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMultiTypeAttributeListDirectory();

// MFT index of the directory record built by
// BuildFakeNtfsImageWithFragmentedAttributeListDirectory().
inline constexpr ULONGLONG uaf_attr_list_dir_idx = 13;

// MFT indices of the four extension records that directory's
// $ATTRIBUTE_LIST relocates $INDEX_ALLOCATION to. Must stay below
// Enum::MftIdx::USER (16); uaf_extension_idx2/3 reuse indices 1 and 2,
// left unused by BuildFakeNtfsImage().
inline constexpr ULONGLONG uaf_extension_idx0 = 14;
inline constexpr ULONGLONG uaf_extension_idx1 = 15;
inline constexpr ULONGLONG uaf_extension_idx2 = 1;
inline constexpr ULONGLONG uaf_extension_idx3 = 2;

// Distinct real_size sentinel written into each extension record's
// $INDEX_ALLOCATION, so a test can tell which record's memory it is
// still reading.
inline constexpr std::array<DWORD, 4> uaf_real_size_sentinels{1024, 2048, 3072,
                                                              4096};

// Same volume as BuildFakeNtfsImage(), plus a directory whose
// $ATTRIBUTE_LIST relocates $INDEX_ALLOCATION into four distinct
// extension records.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithFragmentedAttributeListDirectory();

// Same volume as BuildFakeNtfsImage(), but with the $MFT file record
// zero-filled (magic == 0, not file_record_magic), while $Volume and the
// root directory are left untouched and valid.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithCorruptMftRecord();

// MFT index of the record built by
// BuildFakeNtfsImageWithAttrNameExceedsTotalSize(). Free below
// Enum::MftIdx::USER (16).
inline constexpr ULONGLONG attr_name_exceeds_total_size_record_idx = 4;

// name_offset/name_length BuildFakeNtfsImageWithAttrNameExceedsTotalSize()
// forges for its single $DATA attribute: together they reach past the
// attribute's declared total_size (28), while still landing well inside
// the 1024-byte record buffer.
inline constexpr WORD attr_name_bounds_name_offset = 100;
inline constexpr BYTE attr_name_bounds_name_length = 6;

// Deterministic bytes written at attr_name_bounds_name_offset, exactly
// attr_name_bounds_name_length wide characters (excluding the terminator).
inline constexpr std::wstring_view attr_name_bounds_sentinel = L"PWNED!";

// Same volume as BuildFakeNtfsImage(), plus a record whose single resident
// $DATA attribute declares a name reaching past its own total_size, while
// still landing on known, deterministic bytes.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithAttrNameExceedsTotalSize();

// Well past this fixture's 1024-byte record, but within a WORD's range.
inline constexpr WORD attr_offset_out_of_bounds = 2000;

// Same volume as BuildFakeNtfsImage(), with the root directory's (#5)
// offset_of_attr patched to attr_offset_out_of_bounds.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttrOffsetOutOfBounds();

// Recognizable byte pattern written as this fixture's entire $DATA body.
inline constexpr std::array<BYTE, 4> small_resident_data_content{0xDE, 0xAD,
                                                                 0xBE, 0xEF};

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose sole attribute is a resident $DATA holding exactly
// small_resident_data_content.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithSmallResidentData();

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose resident $ATTRIBUTE_LIST ends in a short read.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttributeListShortRead();

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose resident $ATTRIBUTE_LIST holds a real entry
// (relocating $INDEX_ROOT to index_extension_idx), followed by one whose own
// record_size is nonzero but smaller than the entry header itself - the
// mid-loop record_size bounds check, distinct from a short ReadData().
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall();

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose resident $ATTRIBUTE_LIST holds a single real entry
// (relocating $INDEX_ROOT to index_extension_idx) whose own record_size
// overshoots the attribute's declared size - the post-loop offset-vs-size
// check, reached through AttrList's normal (nullopt) end.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithAttributeListOffsetMismatch();

// MFT index of the second record in a two-way $ATTRIBUTE_LIST cycle with #5.
inline constexpr ULONGLONG attr_list_cycle_ext_idx = 6;

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// and attr_list_cycle_ext_idx each replaced by one whose resident
// $ATTRIBUTE_LIST names the other, forming a resolution cycle.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithAttributeListCycle();

// Real, fixed on-disk size of a nameless $ATTRIBUTE_LIST entry's header.
inline constexpr WORD attribute_list_real_entry_size = 26;

// MFT index of the directory record with a densely-packed $ATTRIBUTE_LIST.
inline constexpr ULONGLONG attr_list_tight_pack_dir_idx = 6;

// MFT index attr_list_tight_pack_dir_idx's $ATTRIBUTE_LIST relocates
// $INDEX_ROOT to.
inline constexpr ULONGLONG attr_list_tight_pack_ext_idx_a = 7;

// MFT index attr_list_tight_pack_dir_idx's $ATTRIBUTE_LIST relocates
// $INDEX_ALLOCATION to.
inline constexpr ULONGLONG attr_list_tight_pack_ext_idx_b = 8;

// real_size written into attr_list_tight_pack_ext_idx_b's $INDEX_ALLOCATION.
inline constexpr DWORD attr_list_tight_pack_real_size = 4096;

// Same volume as BuildFakeNtfsImage(), plus a directory whose resident
// $ATTRIBUTE_LIST packs two entries at the real on-disk entry stride.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory();

// Same volume as BuildFakeNtfsImage(), with the root directory's (#5) file
// record zero-filled so ParseFileRecord(ROOT) fails.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithCorruptRootRecord();

// Smallest MFT index only reachable through ReadFileRecord()'s
// fragmented-$MFT path.
inline constexpr ULONGLONG fragmented_mft_invalid_record_idx = 16;

// Physical LCN where the forged fragmented-$MFT record is placed.
inline constexpr DWORD fragmented_mft_data_run_lcn = 20;

// Same volume as BuildFakeNtfsImage(), except $MFT's DATA attribute has a
// real data run reaching fragmented_mft_invalid_record_idx, whose file record
// is forged with an invalid offset_of_us.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithFragmentedMftInvalidRecord();

// MFT index of the extension record relocated via $ATTRIBUTE_LIST. Below
// Enum::MftIdx::USER (16), so naively reachable.
inline constexpr ULONGLONG mft_data_split_ext_idx = 6;

// MFT index only reachable through that continuation instance.
inline constexpr ULONGLONG mft_data_split_target_idx = 16;

// Physical LCN the continuation maps mft_data_split_target_idx's VCN to.
inline constexpr DWORD mft_data_split_lcn = 40;

// Same volume as BuildFakeNtfsImage(), except $MFT's own DATA attribute is
// split via $ATTRIBUTE_LIST, its continuation covering mft_data_split_target_idx.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList();

// MFT index of the one extension record holding both of $MFT's DATA extents
// below. Below Enum::MftIdx::USER (16), so naively reachable.
inline constexpr ULONGLONG mft_two_extents_ext_idx = 6;

// Start VCNs of the two extents. Far apart, so nothing merges them into one.
inline constexpr ULONGLONG mft_two_extents_first_vcn = 16;
inline constexpr ULONGLONG mft_two_extents_second_vcn = 32;

// Physical LCNs the two extents map their single cluster to.
inline constexpr DWORD mft_two_extents_first_lcn = 40;
inline constexpr DWORD mft_two_extents_second_lcn = 41;

// Same volume as BuildFakeNtfsImage(), except $MFT's $ATTRIBUTE_LIST names one
// extension record twice, for the two extents above. That record holds both
// $DATA attributes. A file record sits at each extent's LCN.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord();

// How an extension record relates to the $ATTRIBUTE_LIST entry naming it.
struct FakeExtensionLink
{
  // Sequence number the list entry carries for the extension record.
  WORD entry_sequence = 0;
  // Sequence number in the extension record's own header.
  WORD record_sequence = 0;
  // Base file reference in the extension record's own header.
  ULONGLONG base_ref = 0;
};

// The extension link of a genuine extension of attr_list_lifetime_base_idx.
inline constexpr FakeExtensionLink genuine_extension_link{
    .entry_sequence = 3,
    .record_sequence = 3,
    // Number 6 with sequence 2, as NTFS packs a file reference.
    .base_ref = 6U | (2ULL << 48U)};

// Same volume as BuildFakeNtfsImage(), plus attr_list_lifetime_base_idx, whose
// resident $ATTRIBUTE_LIST names attr_list_lifetime_ext_idx for $DATA. That
// record holds one resident $DATA of attr_list_lifetime_data_content, and
// relates to the list entry as link says.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithExtensionLink(FakeExtensionLink link);

// Same volume as BuildFakeNtfsImage(), except $MFT's own DATA attribute is
// split via $ATTRIBUTE_LIST like
// BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList(), with the list
// entry and the extension record carrying the sequence numbers in link.
// link.base_ref is ignored: the extension always names $MFT.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMftDataSplitLink(FakeExtensionLink link);

// MFT index of the chain's middle extent, naively reachable.
inline constexpr ULONGLONG mft_chain_ext_b = 7;

// Start VCN of mft_chain_ext_b's continuation.
inline constexpr ULONGLONG mft_chain_ext_b_start_vcn = 100;

// Physical LCN mft_chain_ext_b_start_vcn maps to.
inline constexpr DWORD mft_chain_ext_b_lcn = 200;

// Clusters mft_chain_ext_b's extent covers - must reach mft_chain_ext_a.
inline constexpr DWORD mft_chain_ext_b_clusters = 50;

// MFT index of the final extent, reachable only via mft_chain_ext_b's own extent.
inline constexpr ULONGLONG mft_chain_ext_a = 120;

// Start VCN of mft_chain_ext_a's continuation; also its target record's index.
inline constexpr ULONGLONG mft_chain_ext_a_start_vcn = 500;

// Physical LCN mft_chain_ext_a_start_vcn maps to.
inline constexpr DWORD mft_chain_target_lcn = 300;

// Same volume as BuildFakeNtfsImage(), with a two-entry $ATTRIBUTE_LIST
// where resolving entry 1 needs entry 2's extent known first.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithMftDataExtentChain();

// MFT index of a permanently unresolvable $ATTRIBUTE_LIST entry.
inline constexpr ULONGLONG mft_unresolvable_ext_idx = 300;

// Start VCN $ATTRIBUTE_LIST declares for mft_unresolvable_ext_idx.
inline constexpr ULONGLONG mft_unresolvable_start_vcn = 9000;

// MFT index of the second, resolvable entry.
inline constexpr ULONGLONG mft_unresolvable_good_ext_idx = 9;

// Start VCN of mft_unresolvable_good_ext_idx's continuation.
inline constexpr ULONGLONG mft_unresolvable_good_start_vcn = 200;

// Physical LCN mft_unresolvable_good_start_vcn maps to.
inline constexpr DWORD mft_unresolvable_good_lcn = 400;

// Clusters mft_unresolvable_good_ext_idx's extent covers.
inline constexpr DWORD mft_unresolvable_good_clusters = 10;

// MFT index of the record reachable through mft_unresolvable_good_ext_idx.
inline constexpr ULONGLONG mft_unresolvable_good_record = 205;

// Same volume as BuildFakeNtfsImage(), with one unresolvable entry ahead of
// one resolvable entry - the shape that caused 75a12d9's use-after-free.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithUnresolvableMftDataExtent();

// MFT index the $ATTRIBUTE_LIST entry of the overflow fixture names. At or
// above Enum::MftIdx::USER, so resolving it consults the $MFT extents.
inline constexpr ULONGLONG mft_last_vcn_overflow_target_idx = 16;

// Last VCN forged into $MFT's base DATA attribute: with fake clusters of
// 1024 bytes, (last VCN + 1) * cluster size is exactly 2^64 and wraps to 0.
inline constexpr ULONGLONG mft_last_vcn_overflow_last_vcn =
    std::numeric_limits<ULONGLONG>::max() / fake_cluster_size;

// Same volume as BuildFakeNtfsImage(), except $MFT's base DATA attribute
// claims mft_last_vcn_overflow_last_vcn as its last VCN, and its $ATTRIBUTE_LIST
// names a continuation record past Enum::MftIdx::USER.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithMftDataLastVcnOverflow();

// Shared with NTFSLibTests/fuzz/named-stream-probe.h, so a fuzz corpus file
// built from this fixture reaches the same named stream by name.
using NtfsFuzz::named_data_stream_name;
using NtfsFuzz::named_data_stream_name_length;

// Recognizable byte pattern written as this fixture's entire $DATA body.
inline constexpr std::array<BYTE, 4> named_data_stream_content{0xCA, 0xFE, 0xBA,
                                                               0xBE};

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose sole attribute is a named $DATA stream.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithNamedDataStream();

// MFT indices of the two directory records built by
// BuildFakeNtfsImageWithIndexRootVariants().
inline constexpr ULONGLONG index_root_variant_a_dir_idx = 6;
inline constexpr ULONGLONG index_root_variant_b_dir_idx = 7;

// mft_index each variant's single FILE_NAME entry declares.
inline constexpr ULONGLONG index_root_variant_a_mft_ref = 30;
inline constexpr ULONGLONG index_root_variant_b_mft_ref = 40;

// File names each variant's single FILE_NAME entry declares.
inline constexpr std::wstring_view index_root_variant_a_name = L"AAA";
inline constexpr std::wstring_view index_root_variant_b_name = L"BBB";

// Same volume as BuildFakeNtfsImage(), plus two same-size directory records,
// each holding its own resident $INDEX_ROOT with a distinct FILE_NAME entry.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithIndexRootVariants();

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one holding its own real $INDEX_ROOT entry directly, not via
// an $ATTRIBUTE_LIST extension record.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithRootIndexRootEntry();

// Shared with NTFSLibTests/fuzz/gap-collation-probe.h, so a fuzz corpus
// file built from this fixture reaches the same sub-node entry by name.
using NtfsFuzz::gap_collation_search_name;
using NtfsFuzz::gap_collation_search_name_length;

// MFT reference the root-level, non-terminal $INDEX_ROOT entry declares.
inline constexpr ULONGLONG gap_collation_non_terminal_mft_ref = 25;

// MFT reference the sub-node's real leaf entry declares.
inline constexpr ULONGLONG gap_collation_leaf_mft_ref = 30;

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by one whose own $INDEX_ROOT holds a real, non-terminal entry
// that is also a sub-node pointer into a real $INDEX_ALLOCATION index
// block holding gap_collation_search_name as its leaf entry.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithGapCollationSubNode();

// Where BuildFakeNtfsImageWithNonAsciiNames() files its names.
enum class NonAsciiNameLayout : std::uint8_t
{
  // Leaf entries of the root directory's own $INDEX_ROOT.
  IndexRoot,
  // Leaf entries of an $INDEX_ALLOCATION block the $INDEX_ROOT points at.
  IndexBlock,
};

// Names BuildFakeNtfsImageWithNonAsciiNames() files, in that order: e-acute,
// O-diaeresis and dotless i, each followed by ".txt". Escapes, so the source
// does not depend on the compiler's source character set.
inline constexpr std::wstring_view non_ascii_acute_name = L"\u00E9.txt";
inline constexpr std::wstring_view non_ascii_acute_upper_name = L"\u00C9.txt";
inline constexpr std::wstring_view non_ascii_diaeresis_name = L"\u00D6.txt";
inline constexpr std::wstring_view non_ascii_dotless_name = L"\u0131.txt";
inline constexpr std::wstring_view non_ascii_dotted_upper_name = L"I.txt";

// MFT references those three names declare.
inline constexpr ULONGLONG non_ascii_acute_mft_ref = 31;
inline constexpr ULONGLONG non_ascii_diaeresis_mft_ref = 32;
inline constexpr ULONGLONG non_ascii_dotless_mft_ref = 33;

// Same volume as BuildFakeNtfsImage(), with the root directory (#5) filing
// the three names above, sorted as a real volume would sort them under its
// own $UpCase table: by the uppercase form of each UTF-16 code unit. With
// withUpCase, $UpCase (#10) is a real 128 KiB table that leaves the dotless i
// unmapped, as Windows does. Without it, record 10 does not exist.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithNonAsciiNames(NonAsciiNameLayout layout,
                                        bool with_up_case);

// Chain length for BuildFakeNtfsImageWithDeepIndexBlockChain(), well past
// the depth limit; every VCN is distinct, so no cycle guard can stop it.
inline constexpr DWORD index_block_chain_length = 70;

// File reference the chain's leaf entry declares.
inline constexpr ULONGLONG index_block_chain_leaf_mft_ref = 99;

// Name (and UTF-16 length) of the chain's leaf entry, reachable only by
// descending past every other block first.
inline constexpr std::wstring_view index_block_chain_leaf_name = L"Deep";
inline constexpr BYTE index_block_chain_leaf_name_length = 4;

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by a bare directory whose $INDEX_ALLOCATION chains
// index_block_chain_length index blocks, each pointing to the next, the last
// holding index_block_chain_leaf_name as a real entry.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithDeepIndexBlockChain();

// MFT reference BuildFakeNtfsImageWithOrphanedIndexBlocks() gives each of its
// three leaf entries' mft_index field.
inline constexpr ULONGLONG orphaned_block_reachable_mft_ref = 101;
inline constexpr ULONGLONG orphaned_block_orphan_mft_ref = 102;
inline constexpr ULONGLONG orphaned_block_stale_mft_ref = 103;

// Parent record number BuildFakeNtfsImageWithOrphanedIndexBlocks()'s "Stale"
// entry declares: some directory other than the root, so a parent filter
// must reject it even though its block is otherwise well-formed.
inline constexpr ULONGLONG orphaned_block_stale_parent_ref = 999;

// Names (and UTF-16 lengths) of BuildFakeNtfsImageWithOrphanedIndexBlocks()'s
// three leaf entries: reachable through the normal B+ tree walk, reachable
// only by scanning every $INDEX_ALLOCATION block, and reachable that way but
// filed under a different parent.
inline constexpr std::wstring_view orphaned_block_reachable_name = L"Reachable";
inline constexpr std::wstring_view orphaned_block_orphan_name = L"Orphan";
inline constexpr std::wstring_view orphaned_block_stale_name = L"Stale";

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// a directory whose $INDEX_ROOT points at a single real $INDEX_ALLOCATION
// block (VCN 0, holding orphaned_block_reachable_name), while the stream holds
// two further blocks (VCN 1, 2) no pointer in the tree reaches: one holding
// a normal entry (orphaned_block_orphan_name), the other an entry filed under
// a different parent (orphaned_block_stale_name) - as a deleted file's
// leftover entry would be. Models a directory index whose B+ tree pointers
// were partly lost while the underlying blocks survived.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithOrphanedIndexBlocks();

// Declared block count BuildFakeNtfsImageWithHugeOrphanScanBlockCount()
// forges into its $INDEX_ALLOCATION real_size: past FileRecord's internal
// max_orphan_scan_blocks (65536), while only the same 3 real blocks as
// BuildFakeNtfsImageWithOrphanedIndexBlocks() are ever backed.
inline constexpr ULONGLONG huge_orphan_scan_declared_block_count = 70000;

// Same as BuildFakeNtfsImageWithOrphanedIndexBlocks(), except the root's
// $INDEX_ALLOCATION declares huge_orphan_scan_declared_block_count blocks: the
// orphan scan must cap its work at max_orphan_scan_blocks instead of iterating
// the whole declared (attacker-controlled) count.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithHugeOrphanScanBlockCount();

// MFT index BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch() gives
// its one extra, real record: free (zero-filled) in BuildFakeNtfsImage(),
// and below Enum::MftIdx::USER (16), so it is reachable however $MFT's own
// DATA attribute is mapped.
inline constexpr ULONGLONG orphaned_block_sequence_mismatch_target_idx = 2;

// Sequence number BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch()
// gives that extra record on disk - deliberately different from the
// hardcoded mft_sn (1) WriteOrphanedIndexLeafBlock() gives every leaf entry
// it writes, including the "Orphan" one redirected to name this record.
inline constexpr WORD orphaned_block_sequence_mismatch_record_seq = 7;

// Same volume as BuildFakeNtfsImageWithOrphanedIndexBlocks(), except the
// "Orphan" leaf entry (VCN 1) is redirected to name
// orphaned_block_sequence_mismatch_target_idx instead of
// orphaned_block_orphan_mft_ref: a record that actually exists and is in use,
// but under a sequence number that does not match the entry's own mft_sn.
// Decision 4's other drop condition, alongside "the record doesn't exist"
// (which BuildFakeNtfsImageWithOrphanedIndexBlocks() itself already models).
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch();

// clusters_per_ib BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock()
// declares (via the shared BPB's clusters_per_index_block): more than 1, so
// ScanOrphanedIndexBlocks()'s blockIndex-to-VCN scaling (blockIndex times
// clusters_per_ib) actually multiplies - every other orphan-scan fixture
// hardcodes 1, leaving that multiplication untested.
inline constexpr BYTE multi_cluster_orphan_clusters_per_block = 2;

// MFT reference the fixture's block-0 (reachable) leaf entry declares.
inline constexpr ULONGLONG multi_cluster_reachable_mft_ref = 105;

// MFT reference the fixture's block-1 (orphaned) leaf entry declares.
inline constexpr ULONGLONG multi_cluster_orphan_mft_ref = 106;

// Name (and UTF-16 length) of the block-0 leaf entry, reachable through the
// normal B+ tree walk.
inline constexpr std::wstring_view multi_cluster_reachable_name =
    L"MultiReachable";

// Name (and UTF-16 length) of the block-1 leaf entry: found only if the
// recovery scan converts its block index (1) to VCN
// multi_cluster_orphan_clusters_per_block (2), not VCN 1.
inline constexpr std::wstring_view multi_cluster_orphan_name = L"MultiOrphan";

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// a directory whose index blocks are multi_cluster_orphan_clusters_per_block
// clusters wide: $INDEX_ROOT points only at block 0 (VCN 0,
// multi_cluster_reachable_name), while $INDEX_ALLOCATION also covers block 1
// (VCN multi_cluster_orphan_clusters_per_block, multi_cluster_orphan_name), which
// no B+ tree pointer reaches.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock();

// Names of the four leaf entries, one per index block, of
// BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks(), in block order.
inline constexpr std::array<std::wstring_view, 4> sub_cluster_block_names{
    L"SubBlock0", L"SubBlock1", L"SubBlock2", L"SubBlock3"};

// Same volume as BuildFakeNtfsImage(), with index blocks of 512 bytes, half a
// cluster. The root record (#5) holds a directory whose $INDEX_ROOT points
// only at block 0, while its $INDEX_ALLOCATION maps four blocks. Block i holds
// sub_cluster_block_names[i], filed under the root.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks();

// Names of the four leaf entries, one per index block, of
// BuildFakeNtfsImageWithSplitIndexAllocation(), in block order.
inline constexpr std::array<std::wstring_view, 4> split_block_names{
    L"SplitBlock0", L"SplitBlock1", L"SplitBlock2", L"SplitBlock3"};

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by a
// directory whose $INDEX_ROOT points only at block 0. Its $INDEX_ALLOCATION
// is split into two instances of two clusters each, which merge into one
// stream of four one-cluster blocks. Block i holds split_block_names[i], filed
// under the root.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithSplitIndexAllocation();

// How a directory record relates to an orphaned index block's entry filed
// under it.
struct FakeParentLink
{
  // Sequence number in the entry's parent reference. 0 claims nothing.
  WORD entry_parent_sequence = 0;
  // Sequence number in the directory record's own header.
  WORD record_sequence = 0;
  // Whether the directory record is in use, or freed.
  bool record_in_use = true;
};

// MFT reference, name and UTF-16 length of the entry
// BuildFakeNtfsImageWithOrphanedIndexBlockParentLink() files in its VCN 2
// block.
inline constexpr ULONGLONG orphaned_block_generation_mft_ref = 104;
inline constexpr std::wstring_view orphaned_block_generation_name =
    L"Generation";

// Same volume as BuildFakeNtfsImageWithOrphanedIndexBlocks(), except the entry
// of its VCN 2 block is orphaned_block_generation_name, filed under the root's
// record number with link.entry_parent_sequence, while the root record itself
// carries link.record_sequence and is in use as link says. VCN 0 is reached
// through the tree. VCN 1's entry names the directory with sequence 0, so the
// scan always reports it.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithOrphanedIndexBlockParentLink(FakeParentLink link);

// Record slots BuildFakeNtfsImageWithMftTree()'s $MFT has room for.
inline constexpr ULONGLONG mft_tree_record_count = 26;

// Records of BuildFakeNtfsImageWithMftTree(), all past the system files. See
// that function for what each one tests.
inline constexpr ULONGLONG mft_tree_docs_idx = 16;
inline constexpr ULONGLONG mft_tree_report_idx = 17;
inline constexpr ULONGLONG mft_tree_hard_link_idx = 18;
inline constexpr ULONGLONG mft_tree_deleted_file_idx = 19;
inline constexpr ULONGLONG mft_tree_deleted_dir_idx = 20;
inline constexpr ULONGLONG mft_tree_deleted_child_idx = 21;
inline constexpr ULONGLONG mft_tree_stale_child_idx = 22;
inline constexpr ULONGLONG mft_tree_reused_dir_idx = 23;
inline constexpr ULONGLONG mft_tree_extension_idx = 24;
inline constexpr ULONGLONG mft_tree_zeroed_idx = 25;

// Size of mft_tree_report_idx's resident $DATA. Its $FILE_NAME claims
// mft_tree_report_stale_size instead, the way a file grown since its last
// rename does.
inline constexpr DWORD mft_tree_report_data_size = 37;
inline constexpr ULONGLONG mft_tree_report_stale_size = 999;
// Allocated size of that same resident $DATA: NTFS pads a resident
// attribute record to an 8-byte boundary, and the allocated size is that
// padded record length minus its 24-byte HeaderResident, not the raw
// content length. WriteResidentDataAttr() reproduces the padding, so this
// is AlignAttrSize(24 + mft_tree_report_data_size) - 24 = 64 - 24.
inline constexpr DWORD mft_tree_report_allocated_size = 40;

// Same volume as BuildFakeNtfsImage(), with mft_tree_record_count record slots
// and a $MFT data run over them, holding:
//   5  the root directory, sequence 5;
//   16 "Docs", a directory in the root;
//   17 "report.txt" in Docs, read-only and archive, with a DOS alias
//      "REPORT~1.TXT";
//   18 a file with two hard links: "link-a" in the root, "link-b" in Docs;
//   19 "old.tmp", deleted, in Docs;
//   20 "OldDir", a deleted directory in Docs, its sequence bumped on
//      deletion;
//   21 "draft.doc", deleted, in OldDir under OldDir's pre-deletion sequence;
//   22 "stale.txt", deleted, in record 23 under a sequence 23 no longer has;
//   23 "NewDir", a directory in the root, in use, whose sequence is one past
//      the one 22 names - the "freed" rule MUST NOT apply to a live record;
//   24 an extension record of 17, with a name of its own;
//   25 a zero-filled slot.
// Records 1, 2, 4 and 6-15 are zero-filled as in BuildFakeNtfsImage(), and
// $MFT (0) is named "$MFT" in the root.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithMftTree();

// Same volume as BuildFakeNtfsImageWithMftTree(), with the zero-filled slot
// mft_tree_zeroed_idx replaced by a nameless extension record of $MFT (record
// 0), as a fragmented $MFT has. Its base file reference is $MFT's sequence
// number over record number 0, so its record number alone is 0.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithMftExtensionRecord();

// $MFT real_size BuildFakeNtfsImageWithHugeMftRealSize() declares: the largest
// value the field holds, so no record count derived from it is believable.
inline constexpr ULONGLONG huge_mft_real_size = ~0ULL;

// Same volume as BuildFakeNtfsImageWithMftTree(), with $MFT's $DATA real_size
// forged to huge_mft_real_size while its data run still maps only
// mft_tree_record_count clusters.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithHugeMftRealSize();

// Real on-disk minimum size of a legacy NTFS 1.2 $STANDARD_INFORMATION
// attribute, before the Windows-2000-era owner_id/security_id/quota/usn
// extension appended four more fields.
inline constexpr WORD legacy_standard_information_size = 48;

// MFT index of the record built by
// BuildFakeNtfsImageWithLegacyStandardInformation().
inline constexpr ULONGLONG legacy_standard_information_record_idx = 6;

// Same volume as BuildFakeNtfsImage(), plus a record
// (legacy_standard_information_record_idx) whose only attribute is a resident
// $STANDARD_INFORMATION shrunk to legacy_standard_information_size bytes.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLegacyStandardInformation();

// Same volume as BuildFakeNtfsImage(), with the root directory record (#5)
// replaced by a bare record whose only attribute is a resident
// $STANDARD_INFORMATION shrunk to legacy_standard_information_size bytes, so
// FuzzOnce() (which only ever parses MftIdx::ROOT) can reach it directly.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithLegacyStandardInformationOnRoot();

// Same volume as BuildFakeNtfsImage(), plus a record
// (legacy_standard_information_record_idx) whose only attribute is a resident
// $STANDARD_INFORMATION declaring no body at all (attr_size 0).
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithEmptyStandardInformation();

////////////////////////////////////////////////////////////////////////////
// NTFS compression (FILE_ATTRIBUTE_COMPRESSED + comp_unit_size + LZNT1)
////////////////////////////////////////////////////////////////////////////

// One data run: "clusters" clusters at LCN "lcn", or a sparse hole when
// "lcn" is empty. Encodes into NTFS' real run-list format, so fixtures can
// describe fragmented/sparse layouts. "clusters" must fit one length byte.
struct FakeDataRun
{
  std::optional<DWORD> lcn;
  DWORD clusters = 0;
};

// Compression unit exponent every fixture uses: 4 clusters, one chunk.
inline constexpr WORD compression_unit_size_shift = 2;

// Clusters and bytes per compression unit implied by
// compression_unit_size_shift.
inline constexpr DWORD compression_unit_clusters =
    1U << compression_unit_size_shift;
inline constexpr DWORD compression_unit_size =
    compression_unit_clusters * fake_cluster_size;

// LCN where compression fixtures place real cluster data, clear of the
// older fixtures' index-block LCNs (20, 100).
inline constexpr DWORD compressed_data_lcn = 30;

// Second, non-contiguous LCN so a fragmented unit's compressed bytes span
// two Data::RunEntrys instead of one.
inline constexpr DWORD fragmented_compressed_data_lcn = 35;

// [MS-XCA] section 3.3's worked example: 59 bytes of real LZNT1-compressed
// data, decompressing to xca_lznt1_example_decompressed. Used verbatim so no
// compressor need be written in this read-only-library repo.
inline constexpr std::array<BYTE, 59> xca_lznt1_example_compressed{
    0x38, 0xb0, 0x88, 0x46, 0x23, 0x20, 0x00, 0x20, 0x47, 0x20, 0x41, 0x00,
    0x10, 0xa2, 0x47, 0x01, 0xa0, 0x45, 0x20, 0x44, 0x00, 0x08, 0x45, 0x01,
    0x50, 0x79, 0x00, 0xc0, 0x45, 0x20, 0x05, 0x24, 0x13, 0x88, 0x05, 0xb4,
    0x02, 0x4a, 0x44, 0xef, 0x03, 0x58, 0x02, 0x8c, 0x09, 0x16, 0x01, 0x48,
    0x45, 0x00, 0xbe, 0x00, 0x9e, 0x00, 0x04, 0x01, 0x18, 0x90, 0x00};

// The ANSI string xca_lznt1_example_compressed decompresses to ([MS-XCA]
// section 3.3); the byte count is size() + 1 since the terminal NUL is part
// of the data.
inline constexpr std::string_view xca_lznt1_example_decompressed =
    "F# F# G A A G F# E D D E F# F# E E F# F# G A A G F# E D D E F# E D D E E "
    "F# D E F# G F# D E F# G F# E D E A F# F# G A A G F# E D D E F# E D D";

inline constexpr size_t xca_lznt1_example_decompressed_size =
    xca_lznt1_example_decompressed.size() + 1;
// Decompressed size [MS-XCA] section 3.3 states for its worked example.
inline constexpr size_t xca_lznt1_example_expected_size = 142;
static_assert(xca_lznt1_example_decompressed_size ==
                  xca_lznt1_example_expected_size,
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

// Clusters of BuildFakeNtfsImageWithUninitializedTail()'s $DATA stream.
inline constexpr DWORD uninitialized_tail_clusters = 3;

// That stream's real size: the three clusters, short of 72 bytes.
inline constexpr ULONGLONG uninitialized_tail_real_size = 3000;

// That stream's initialized size. 1500 falls inside the second cluster, so
// the boundary is deliberately not cluster-aligned.
inline constexpr ULONGLONG uninitialized_tail_ini_size = 1500;

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by a
// plain, uncompressed $DATA stream whose initialized size is below its real
// size. Every cluster holds CompressionFixturePattern() residue, so a read
// past the initialized size must yield zeros, not that residue.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithUninitializedTail();

// Same as BuildFakeNtfsImageWithCompressedFile(), but the unit is *stored*:
// real runs cover it fully (no sparse pad), so its plain bytes
// (CompressionFixturePattern(compression_unit_size)) must pass through as-is.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithStoredCompressionUnit();

// Clusters of BuildFakeNtfsImageWithMultiClusterBitmap()'s $BITMAP stream.
inline constexpr DWORD multi_cluster_bitmap_clusters = 3;

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by a
// bare record whose non-resident $BITMAP spans multi_cluster_bitmap_clusters
// whole clusters. Its first cluster is all ones (every tracked cluster used)
// and its second all zeros (all free). The third is all zeros except bit 0,
// set.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithMultiClusterBitmap();

// Same as BuildFakeNtfsImageWithCompressedFile(), but the unit is a pure
// hole (no real cluster); must read back as compression_unit_size zeroes.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithSparseCompressionUnit();

// Payload size needing two compressed clusters, still under one unit.
inline constexpr size_t fragmented_compressed_payload_size = 2000;

// Same as BuildFakeNtfsImageWithCompressedFile(), but the unit's compressed
// bytes live in two non-contiguous real runs plus a sparse pad, so reading
// it must stitch fragments together before decompressing.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithFragmentedCompressedFile();

// Sizes of the fixture's two units: a full stored one, then a short one.
inline constexpr size_t trailing_partial_unit_stored_size =
    compression_unit_size;
inline constexpr size_t trailing_partial_unit_tail_size = 500;

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
inline constexpr size_t short_decompressed_unit_size = 100;

// Same as BuildFakeNtfsImageWithCompressedFile(), but two units long and the
// first (interior) unit's LZNT1 stream stops early; ReadData() must reject
// it instead of zero-padding, unlike a legitimately short trailing unit.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithShortDecompressedUnit();

// One byte short of the base header plus the CompressedSize field.
inline constexpr DWORD compressed_attr_truncated_total_size = 71;

// Same as BuildFakeNtfsImageWithCompressedFile(), with total_size forced to
// compressed_attr_truncated_total_size: passes the base non-resident size gate
// but not the CompressedSize field comp_unit_size implies must be present.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithCompressedAttrMissingCompressedSize();

// comp_unit_size BuildFakeNtfsImageWithCompUnitSizeOutOfRange() declares: a
// shift so large that 1ULL << comp_unit_size would be undefined behaviour.
inline constexpr WORD comp_unit_size_out_of_range_shift = 64;

// Same as BuildFakeNtfsImageWithCompressedFile(), with comp_unit_size set to
// comp_unit_size_out_of_range_shift; AttrNonResident<S>'s constructor must reject
// the shift before ever using it.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithCompUnitSizeOutOfRange();

// A well-defined shift (2048 clusters) past the largest buffered unit size.
inline constexpr WORD oversized_comp_unit_size_shift = 11;

// Same as BuildFakeNtfsImageWithCompressedFile(), with comp_unit_size set to
// oversized_comp_unit_size_shift, exceeding the constructor's size cap.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithOversizedCompressionUnit();

// Not a multiple of compression_unit_clusters.
inline constexpr ULONGLONG misaligned_compressed_start_vcn = 2;

// Same as BuildFakeNtfsImageWithCompressedFile(), with start_vcn set to
// misaligned_compressed_start_vcn; units are indexed relative to start_vcn, so
// a misaligned one must be rejected, not decoded against a shifted window.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMisalignedCompressedStartVcn();

// LCN where a non-resident $EFS stream lives, clear of the data streams.
inline constexpr DWORD fake_efs_stream_lcn = 60;

// One non-resident $DATA stream of an encrypted fake file. The bytes are
// stored as given: encrypting them is the test's job.
struct FakeEncryptedStream
{
  std::wstring name;                // empty: the unnamed stream
  std::vector<FakeDataRun> runs;    // its layout, sparse holes included
  std::vector<BYTE> cluster_bytes;  // laid over the real runs, in order
  ULONGLONG real_size{0};
  // Initialized size. Nullopt: the same as real_size.
  std::optional<ULONGLONG> ini_size;
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
  // The resident $EFS declares a body larger than its own attribute: its
  // constructor rejects it. Needs efs_resident.
  bool efs_body_overruns{false};
  // A malformed attribute after the last valid one: its total_size is
  // smaller than a resident attribute's own header.
  bool trailing_undersized_attribute{false};
};

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// the file described.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithEncryptedFile(const FakeEncryptedFile& file);

// Byte pattern BuildFakeNtfsImageWithResidentEncryptedData() writes as its
// resident $DATA body; arbitrary, since decryption is never attempted on it.
inline constexpr std::array<BYTE, 4> resident_encrypted_data_content{
    0x11, 0x22, 0x33, 0x44};

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// one whose sole attribute is a RESIDENT $DATA carrying the EFS "encrypted"
// attribute-header flag (Efs::attr_flag_encrypted) - real NTFS never
// encrypts a resident stream, but AttachEfsContext() must still handle a
// forged one.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithResidentEncryptedData();

// Names, MFT references of the two entries of the encrypted directory below:
// the first sits in its $INDEX_ROOT, the second (a directory) in its index
// block.
inline constexpr std::array<std::wstring_view, 2> encrypted_directory_names{
    L"secret.txt", L"vault"};
inline constexpr std::array<ULONGLONG, 2> encrypted_directory_mft_refs{40, 41};

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
inline constexpr std::wstring_view compressed_index_entry_name = L"Comp";
inline constexpr BYTE compressed_index_entry_name_length = 4;

// mft reference that same entry declares.
inline constexpr ULONGLONG compressed_index_entry_mft_ref = 55;

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
inline constexpr std::array<std::wstring_view, 4> surrogate_names{
    L"\U00013080", L"\U0001F41C",
    L"\U0001F468\u200D\U0001F469\u200D\U0001F467\u200D\U0001F466",
    L"\U00020BB7"};

// Whether each entry above is a directory: each node holds one directory
// and one file.
inline constexpr std::array<bool, 4> surrogate_name_is_directory{false, true,
                                                                 true, false};

// MFT references the four entries above point at, in the same order.
inline constexpr std::array<ULONGLONG, 4> surrogate_name_mft_refs{56, 57, 58,
                                                                  59};

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// a compressed directory whose entries are surrogate_names: the first two
// resident in $INDEX_ROOT, the last two inside the LZNT1-wrapped index block
// $INDEX_ALLOCATION decompresses to. Reachable through FuzzOnce().
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithSurrogatePairNames();

////////////////////////////////////////////////////////////////////////////
// Fuzz-corpus-only compressed $INDEX_ALLOCATION fixtures: same recipe as
// BuildFakeNtfsImageWithCompressedIndexAllocation(), since FuzzOnce() only
// ever ReadData()s through $INDEX_ROOT/$INDEX_ALLOCATION, never plain $DATA.
////////////////////////////////////////////////////////////////////////////

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), with
// comp_unit_size set to comp_unit_size_out_of_range_shift instead - rejected
// before the data run is ever parsed.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithCompUnitSizeOutOfRangeIndexAllocation();

// Same as above, with comp_unit_size set to oversized_comp_unit_size_shift: a
// well-defined shift past the largest unit size buffered for.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithOversizedCompressionUnitIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), with start_vcn
// forced to misaligned_compressed_start_vcn via FakeNonResidentOverrides.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMisalignedStartVcnIndexAllocation();

// Same as BuildFakeNtfsImageWithCompressedIndexAllocation(), with total_size
// forced to compressed_attr_truncated_total_size - a ParseAttrs()-level
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
// real cluster decompresses to only short_decompressed_unit_size bytes, far
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

// One chunk decompressing to exactly chunk_size (4096) bytes, with one more
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

////////////////////////////////////////////////////////////////////////////
// VolumeOptions matrix-row fixtures: a defect strict mode rejects whole and
// recover_errors salvages, each built directly (not through the fuzz
// corpus), so a unit test can assert on the parsed result.
////////////////////////////////////////////////////////////////////////////

// LCNs BuildFakeNtfsImageWithBadDataRun() gives its two data runs. Never
// actually read: the run list is rejected while still being decoded.
inline constexpr DWORD bad_data_run_first_lcn = 70;
inline constexpr DWORD bad_data_run_second_lcn = 71;

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// one whose non-resident $DATA attribute's data run decodes a first, real
// run (VCN 0) cleanly, then hits a decode error on the second run: the
// attribute's own last_vcn is forged to VCN 0, one short of what the second
// run's VCN range would need - AttrNonResident<S>::ParseDataRun()'s "VCN
// exceeds bound" check, with one real run already parsed when it fires.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithBadDataRun();

// Names and MFT references BuildFakeNtfsImageWithBadIndexBlockEntry() gives
// its two index blocks' single real entry each: "First" in the damaged
// block (VCN 0), reported only when recovering; "Good" in the sibling,
// well-formed block (VCN 1), reported either way.
inline constexpr ULONGLONG bad_index_block_first_mft_ref = 111;
inline constexpr std::wstring_view bad_index_block_first_name = L"First";
inline constexpr ULONGLONG bad_index_block_good_mft_ref = 112;
inline constexpr std::wstring_view bad_index_block_good_name = L"Good";

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// a directory whose $INDEX_ROOT points directly at two $INDEX_ALLOCATION
// blocks (VCN 0 and VCN 1: both real B+ tree children, reached by the
// normal walk without any recovery). VCN 0 holds one real entry ("First")
// followed by an entry whose declared size overruns the block; VCN 1 is an
// ordinary, well-formed block holding "Good". Models one damaged block next
// to a healthy sibling.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithBadIndexBlockEntry();

// MFT reference BuildFakeNtfsImageWithMalformedIndexEntryFilename()'s single
// entry declares.
inline constexpr ULONGLONG malformed_index_entry_mft_ref = 113;

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// a directory whose $INDEX_ROOT holds a single, terminal entry: a real,
// 3-character name is written on disk, but the entry's own name_length
// field claims 200 characters, reaching past the entry's declared size -
// ValidateIndexEntry()'s "Filename name exceeds entry bounds" check.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithMalformedIndexEntryFilename();

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// one whose sole resident $DATA attribute's total_size reaches exactly to
// the end of the file record, leaving no room for a trailing AttrType::ALL
// end-of-attributes marker - the attribute walk runs out of record before
// ever finding one.
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithNoEndMarker();

// Same volume as BuildFakeNtfsImage(), with $Volume's (#3) record header
// flags cleared (no INUSE bit): a freed $Volume record. NtfsVolume::Init()
// marks its own internal FileRecord read of this record
// (bypass_deleted_gate_) so the volume still opens under default
// VolumeOptions (include_deleted off).
[[nodiscard]] std::vector<BYTE> BuildFakeNtfsImageWithDeletedVolumeRecord();

////////////////////////////////////////////////////////////////////////////
// $ATTRIBUTE_LIST fixtures where the imported attributes' owner record
// (an extension record) must outlive the parse of the record that imports
// them.
////////////////////////////////////////////////////////////////////////////

// MFT index of the base record every fixture below builds.
inline constexpr ULONGLONG attr_list_lifetime_base_idx = 6;

// MFT index of the extension record holding the imported resident $DATA.
inline constexpr ULONGLONG attr_list_lifetime_ext_idx = 7;

// Body of that resident $DATA: 16 distinct bytes, none of them 0xDD (what a
// debug heap writes over freed memory) or 0x00, so a stale read cannot pass.
inline constexpr std::array<BYTE, 16> attr_list_lifetime_data_content{
    0x11, 0x2A, 0x3B, 0x4C, 0x5D, 0x6E, 0x7F, 0x80,
    0x91, 0xA2, 0xB3, 0xC4, 0xD5, 0xE6, 0xF7, 0x08};

// Same volume as BuildFakeNtfsImage(), plus a base record whose resident
// $ATTRIBUTE_LIST first imports the extension record's $DATA, then has an
// entry with a zero record_size: AttrList's constructor throws after having
// moved the imported attribute into the base record.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithAttributeListImportThenZeroRecordSize();

// Same volume as BuildFakeNtfsImage(), plus a base record with two unnamed,
// non-resident $ATTRIBUTE_LIST attributes covering VCN 0-0 and VCN 1-1: an
// empty first one, and a second one whose single entry imports the extension
// record's $DATA. Their contiguous VCN ranges make them look like the two
// halves of one split attribute.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithSplitAttributeListAttribute();

// A malformed attribute written right after the last valid one of a record.
enum class FakeTrailingDefect : std::uint8_t
{
  // total_size is smaller than a resident attribute's own header.
  UndersizedHeader,
  // A compressed non-resident attribute whose total_size leaves no room for
  // its CompressedSize field.
  UndersizedCompressedField,
  // A $STANDARD_INFORMATION marked non-resident: its constructor rejects it.
  RejectedAttribute,
};

// Same volume as BuildFakeNtfsImage(), plus a base record whose unnamed
// $DATA (VCN 0) has a continuation (VCN 1) in the extension record, reached
// through a resident $ATTRIBUTE_LIST, followed by "defect".
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithSplitDataAndTrailingDefect(FakeTrailingDefect defect);

// Where a fixture with a forged run list puts its stream.
enum class FakeRunHost : std::uint8_t
{
  // A plain non-resident $DATA of the root record.
  Data,
  // The $INDEX_ALLOCATION of a root directory: the harness's own read path.
  IndexAllocation,
};

// LCN whose product with fake_cluster_size is exactly 2^64. Computed in
// unsigned 64 bits that product wraps to 0, so a read at this LCN lands on
// the boot sector instead of failing. Needs a power-of-two cluster size.
inline constexpr ULONGLONG wrapping_lcn =
    ((1ULL << 63U) / fake_cluster_size) * 2;

// Same volume as BuildFakeNtfsImage(), with a 1-cluster stream, of the
// given kind, whose only run sits at wrapping_lcn. Reading it MUST fail.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithWrappingLcn(FakeRunHost host);

// Same volume as BuildFakeNtfsImage(), with a 2-cluster stream, of the
// given kind, whose two runs each move the LCN forward by LLONG_MAX. The
// second run's cumulative LCN overflows a signed 64-bit integer.
[[nodiscard]] std::vector<BYTE>
    BuildFakeNtfsImageWithOverflowingLcnSum(FakeRunHost host);

}  // namespace NtfsBrowserTests
