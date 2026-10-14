#pragma once

#include <ntfs-browser/win-types.h>

#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/attr/type.h>

#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/filename-flag.h"
#include "data/filename-namespace.h"
#include "data/index-block.h"
#include "data/index-entry.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image.h"
#include "mft/file-reference.h"

namespace NtfsBrowserTests {

namespace Attr = NtfsBrowser::Attr;
using NtfsBrowser::Data::FileRecordHeader;
using NtfsBrowser::Data::index_block_magic;
namespace Mft = NtfsBrowser::Mft;

// Reuses fake-ntfs-image.h's geometry, so fixture constants declared there
// (eg. compression_unit_size) can be sized in real clusters.
inline constexpr WORD bytes_per_sector = fake_bytes_per_sector;

inline constexpr BYTE sectors_per_cluster = fake_sectors_per_cluster;

inline constexpr DWORD cluster_size = fake_cluster_size;

// $MFT sits in the first cluster after the boot sector.
inline constexpr ULONGLONG mft_lcn = 1;

// Right after Data::FileRecordHeader's fixed header fields.
inline constexpr WORD attr_offset_value = 48;

// Size of the fixup slot a block ends with: the USN, then one word per sector.
inline constexpr WORD us_slot_size = 6;

// Room an attribute reserves after its header for a short data run list.
inline constexpr size_t run_list_room = 8;

// Data run headers: the high nibble is the LCN offset field size, the low
// nibble the length field size. A 4-byte offset and a 1-byte length, or an
// 8-byte offset and a 1-byte length, or a 1-byte offset and a 1-byte length.
inline constexpr BYTE run_header4_lcn_bytes = 0x41;

inline constexpr BYTE run_header8_lcn_bytes = 0x81;

inline constexpr BYTE run_header1_lcn_bytes = 0x11;

// Alignment the readers need to bind structs onto a record.
inline constexpr size_t record_alignment = 8;

// $STANDARD_INFORMATION timestamps whose bytes are all distinct, so a
// misplaced field cannot match by accident.
inline constexpr ULONGLONG std_info_create_time = 0x0102030405060708ULL;

inline constexpr ULONGLONG std_info_alter_time = 0x1112131415161718ULL;

inline constexpr ULONGLONG std_info_mft_time = 0x2122232425262728ULL;

inline constexpr ULONGLONG std_info_read_time = 0x3132333435363738ULL;

// The file reference of the first fake index entry.
inline constexpr DWORD first_entry_record = 20;

inline constexpr unsigned byte_mask = 0xFFU;

inline constexpr unsigned bits_per_byte = 8U;

// How pattern bytes derive from an index: (i * mul + add) % mod. The modulus
// is prime and not a power of two, so no block size or cluster size repeats
// the pattern.
inline constexpr unsigned pattern_mul = 31U;

inline constexpr unsigned pattern_add = 7U;

inline constexpr unsigned pattern_mod = 251U;

// The readers bind structs (8-byte aligned at most) onto offsets inside a
// record, so the record itself must start on that boundary: a plain
// std::array<BYTE> may sit at any stack address.
struct alignas(record_alignment) FakeRecord
    : std::array<BYTE, fake_file_record_size> {};

// Packs an on-disk file reference: record number low, sequence number high.
constexpr ULONGLONG MakeFileReference(ULONGLONG record, WORD sequence) {
  return (static_cast<ULONGLONG>(sequence)
          << NtfsBrowser::Mft::mft_sequence_shift) |
         record;
}

// Sequence number a real volume's root directory record carries; non-zero,
// so a reader that forgets to mask it off a parent_ref sees a wrong number.
inline constexpr WORD root_sequence_number = 5;

// NTFS starts every attribute of a record, and every entry inside one, on an
// 8-byte boundary; the readers bind structs onto these offsets, so an odd one
// is a misaligned access.
inline constexpr DWORD attr_alignment = 8;

// Returns the sub-node VCN slot: the last 8 bytes of the index entry `e`.
ULONGLONG& SubNodeVcnSlot(NtfsBrowser::Data::IndexEntry& index_entry);

// Rounds a size up to attr_alignment.
constexpr DWORD AlignAttrSize(size_t size) {
  return gsl::narrow<DWORD>((size + attr_alignment - 1) &
                            ~(attr_alignment - 1));
}

// Builds a bare file-record header with the given attribute offset and
// flags.
FakeRecord MakeRecordHeader(WORD offset_of_attr,
                            NtfsBrowser::Data::FileRecordFlag flags);

// Makes record an extension record: its own sequence number, and the file
// reference of the base record it belongs to.
void SetRecordLink(FakeRecord& record, WORD sequence, ULONGLONG base_ref);

// Writes the Attr::Type::All end-of-attributes marker at offset.
void WriteEndOfAttributesMarker(FakeRecord& record, DWORD offset);

// Encodes text as on-disk UTF-16LE code units: WORD, not wchar_t, which is
// only 16 bits on some platforms (eg. Windows) and 32 on others (eg. Linux).
// A code point above the BMP is split into a surrogate pair; an element
// already in the surrogate range (eg. one half of a pair a 16-bit wchar_t
// already split) passes through as one unit, so this is correct whether
// text arrived pre-split or not.
std::vector<WORD> ToUtf16(std::wstring_view text);

// Writes record at byteOffset, growing image to the next 64 KiB boundary -
// FullCache always reads a whole 64 KiB block, so a short image short-reads.
void PutRecordAt(std::vector<BYTE>& image, size_t byte_offset,
                 const FakeRecord& record);

// Writes record at $MFT index idx - a plain contiguous slot, mftAddr plus
// idx file records - growing image first if needed.
void PutMftRecord(std::vector<BYTE>& image, DWORD mft_addr, ULONGLONG idx,
                  const FakeRecord& record);

// Builds a fake $MFT record whose DATA attribute has a real, non-empty
// data run of clusters clusters starting at lcn, unlike MakeMftRecord()'s
// empty one.
FakeRecord MakeMftRecordWithRealDataRun(DWORD lcn, DWORD clusters);

// Extension record holding the resident $INDEX_ROOT that
// attribute_list_dir_idx's $ATTRIBUTE_LIST points to: a single "Foo" entry
// (file reference 20), plus the terminating nameless entry. baseIdx is the
// record it extends; 0 leaves it a base record.
FakeRecord MakeIndexRootExtensionRecord(ULONGLONG base_idx = 0);

// Writes one resident $STANDARD_INFORMATION attribute at record[offset]
// with the given DOS permission bits, and returns its total_size.
DWORD WriteStandardInformationAttr(
    FakeRecord& record, DWORD offset,
    NtfsBrowser::Data::StdInfoPermission permission);

// Header fields the deliberately malformed compression fixtures forge -
// values a well-formed builder never derives from its own run list.
struct FakeNonResidentOverrides {
  // Non-zero models one $ATTRIBUTE_LIST fragment of a split attribute.
  ULONGLONG start_vcn = 0;
  // Declares a VCN range wider than the runs map - ParseDataRun()'s state
  // after stopping early on a decode error.
  std::optional<ULONGLONG> last_vcn;
  // Overrides "header + run list bytes" as the attribute's total_size.
  std::optional<DWORD> total_size;
  // A stream name, written between the header and the run list.
  std::wstring_view name;
  // Overrides the flags a compressed/plain attribute would get.
  std::optional<WORD> flags;
  // Overrides the initialized size, which defaults to the real size.
  std::optional<ULONGLONG> ini_size;
  // Hand-encoded run list, terminator included, written instead of the
  // encoding of "runs": for LCNs EncodeDataRuns() cannot express.
  std::vector<BYTE> raw_runs;
};

// Layout of one non-resident attribute, without its type.
struct FakeNonResidentAttr {
  // Compression unit shift the header declares. Zero is an ordinary attribute.
  WORD comp_unit_size = 0;
  // Size of the stream the attribute holds, in bytes.
  ULONGLONG real_size = 0;
  // Clusters the attribute maps, sparse holes included.
  std::vector<FakeDataRun> runs;
  // Malformed header fields a well-formed builder never derives.
  FakeNonResidentOverrides overrides;
};

// Writes one non-resident attribute of type at record[offset] and returns its
// declared total_size. A non-zero comp_unit_size adds the trailing 8-byte
// CompressedSize field.
DWORD WriteNonResidentAttr(FakeRecord& record, DWORD offset, Attr::Type type,
                           const FakeNonResidentAttr& attr);

// File record (root, #5): resident $STANDARD_INFORMATION("permission") plus
// one non-resident $DATA attribute described by compUnitSize/realSize/runs.
FakeRecord
    MakeNonResidentDataRecord(NtfsBrowser::Data::StdInfoPermission permission,
                              WORD comp_unit_size, ULONGLONG real_size,
                              const std::vector<FakeDataRun>& runs,
                              const FakeNonResidentOverrides& overrides = {});

// One leaf $FILE_NAME index entry a fixture writes: a name, the MFT record
// it points at, and whether that record is a directory.
struct FakeIndexName {
  std::wstring_view name;
  ULONGLONG mft_ref;
  bool directory;
};

// Writes "name" as a leaf index entry at "dest" and returns its size in
// bytes. Entries are packed with no padding, like every other fixture here.
WORD WriteFilenameEntry(BYTE* dest, const FakeIndexName& name);

// Writes one resident $INDEX_ROOT at record[offset]: root_names as leaf
// entries, then a nameless SUBNODE-only entry. Returns its total_size.
DWORD WriteIndexRootAttr(FakeRecord& record, DWORD offset,
                         std::span<const FakeIndexName> root_names);

// Directory record (root, #5): resident $STANDARD_INFORMATION("permission"),
// a resident $INDEX_ROOT holding "rootNames" as leaf entries followed by a
// nameless SUBNODE-only entry, and a non-resident $INDEX_ALLOCATION described
// by index_alloc - lets fixtures target an attribute FuzzOnce() actually
// ReadData()s, unlike plain $DATA.
FakeRecord MakeIndexAllocationDirRecord(
    NtfsBrowser::Data::StdInfoPermission permission,
    const FakeNonResidentAttr& index_alloc,
    std::span<const FakeIndexName> root_names = {});

// Lays "clusterBytes" down over the real runs of "runs", in run order, growing
// "image" to hold them; sparse runs consume no bytes and stay zero-filled.
void LayRunBytes(std::vector<BYTE>& image, const std::vector<FakeDataRun>& runs,
                 const std::vector<BYTE>& cluster_bytes);

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// "record" and "clusterBytes" laid down over its real runs, in run order;
// sparse runs consume no bytes and stay zero-filled.
std::vector<BYTE> BuildCompressionImage(const FakeRecord& record,
                                        const std::vector<FakeDataRun>& runs,
                                        const std::vector<BYTE>& cluster_bytes);

// The malformed LZNT1 bytes both corrupt-compression fixtures use: a
// well-formed compressed chunk header, then a compressed word whose
// displacement (1) reaches before anything has been decompressed yet.
std::vector<BYTE> MakeCorruptLznt1Chunk();

// The 1024-byte "INDX"-signed index block a compressed $INDEX_ALLOCATION
// decompresses to: "names" as leaf FILE_NAME entries plus the terminating
// entry. Built standalone since it is the *decompressed* content, wrapped
// into an LZNT1 chunk by the caller.
std::vector<BYTE> MakeIndexBlockContent(std::span<const FakeIndexName> names);

// The single "Comp" entry every compressed $INDEX_ALLOCATION fixture but
// the surrogate-pair one decompresses to.
std::vector<BYTE> MakeCompressedIndexBlockContent();

// One $FILE_NAME of a BuildFakeNtfsImageWithMftTree() record.
struct FakeFileName {
  std::wstring_view name;
  ULONGLONG parent_ref;
  NtfsBrowser::Data::FilenameNamespace name_space =
      NtfsBrowser::Data::FilenameNamespace::Win32;
  // The size NTFS only refreshes in $FILE_NAME on a rename.
  ULONGLONG real_size = 0;
  // ORed onto the DIRECTORY/NONE flag WriteFileNameAttr() derives on its own,
  // eg. for a permission bit $FILE_NAME mirrors from $STANDARD_INFORMATION.
  NtfsBrowser::Data::FilenameFlag extra_flags =
      NtfsBrowser::Data::FilenameFlag::None;
};

// Writes one resident $FILE_NAME at record[offset] and returns its
// total_size.
DWORD WriteFileNameAttr(FakeRecord& record, DWORD offset,
                        const FakeFileName& name, bool directory);

// Writes one resident, unnamed $DATA of size zero bytes at record[offset]
// and returns its total_size.
DWORD WriteResidentDataAttr(FakeRecord& record, DWORD offset, DWORD size);

// Builds one record of BuildFakeNtfsImageWithMftTree(): a header carrying
// sequence and baseRef, a $STANDARD_INFORMATION, names, and, when dataSize is
// set, a resident $DATA of that size.
FakeRecord MakeMftTreeRecord(NtfsBrowser::Data::FileRecordFlag flags,
                             WORD sequence,
                             std::initializer_list<FakeFileName> names,
                             std::optional<DWORD> data_size = {},
                             NtfsBrowser::Data::StdInfoPermission permission =
                                 NtfsBrowser::Data::StdInfoPermission::Normal,
                             ULONGLONG base_ref = 0);

// Writes a malformed attribute at record[offset], as defect describes it.
void WriteTrailingDefect(FakeRecord& record, DWORD offset,
                         FakeTrailingDefect defect);

}  // namespace NtfsBrowserTests
