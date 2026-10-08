#include "fake-ntfs-image.h"

#include <ntfs-browser/win-types.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/data/attr-header-common.h>
#include <ntfs-browser/data/attr-type.h>
#include <ntfs-browser/mft-idx.h>

#include "attr/attribute-list.h"
#include "attr/filename.h"
#include "attr/header-non-resident.h"
#include "attr/header-resident.h"
#include "attr/index-root.h"
#include "attr/standard-information.h"
#include "attr/volume-information.h"
#include "data/file-record-header.h"
#include "data/index-block.h"
#include "data/index-entry.h"
#include "data/ntfs-bpb.h"
#include "efs/efs-context.h"
#include "file-record-header-edit.h"
#include "flag/file-record.h"
#include "flag/filename-namespace.h"
#include "flag/filename.h"
#include "flag/index-entry.h"
#include "flag/std-info-permission.h"
#include "lznt1/decompress.h"
#include "mft-file-reference.h"

namespace NtfsBrowserTests {

using NtfsBrowser::AttrType;
using NtfsBrowser::file_record_magic;
using NtfsBrowser::FileRecordHeader;
using NtfsBrowser::Enum::MftIdx;

namespace {

// Reuses fake-ntfs-image.h's geometry, so fixture constants declared there
// (eg. compression_unit_size) can be sized in real clusters.
constexpr WORD bytes_per_sector = fake_bytes_per_sector;
constexpr BYTE sectors_per_cluster = fake_sectors_per_cluster;
constexpr DWORD cluster_size = fake_cluster_size;
// $MFT sits in the first cluster after the boot sector.
constexpr ULONGLONG mft_lcn = 1;
// Right after FileRecordHeader::Data's fixed header fields.
constexpr WORD attr_offset_value = 48;

// Size of the fixup slot a block ends with: the USN, then one word per sector.
constexpr WORD us_slot_size = 6;

// Points the fixup slot at the record's own last bytes, so PatchUS()
// succeeds without a real fixup array.
constexpr WORD offset_of_us_value = fake_file_record_size - us_slot_size;

// Room an attribute reserves after its header for a short data run list.
constexpr size_t run_list_room = 8;

// Data run headers: the high nibble is the LCN offset field size, the low
// nibble the length field size. A 4-byte offset and a 1-byte length, or an
// 8-byte offset and a 1-byte length.
constexpr BYTE run_header4_lcn_bytes = 0x41;
constexpr BYTE run_header8_lcn_bytes = 0x81;

// Alignment the readers need to bind structs onto a record.
constexpr size_t record_alignment = 8;

// $STANDARD_INFORMATION timestamps whose bytes are all distinct, so a
// misplaced field cannot match by accident.
constexpr ULONGLONG std_info_create_time = 0x0102030405060708ULL;
constexpr ULONGLONG std_info_alter_time = 0x1112131415161718ULL;
constexpr ULONGLONG std_info_mft_time = 0x2122232425262728ULL;
constexpr ULONGLONG std_info_read_time = 0x3132333435363738ULL;

// The file reference of the first fake index entry.
constexpr DWORD first_entry_record = 20;

// A byte no fixture otherwise holds, to tell a forged one from the original.
constexpr BYTE forged_byte = 0xFF;

// A bitmap byte with every bit set, and the mask and shift that split a
// WORD into bytes.
constexpr BYTE all_bits_set = 0xFF;
constexpr unsigned byte_mask = 0xFFU;
constexpr unsigned bits_per_byte = 8U;

// The index block size field's encoding of a size below one cluster:
// 0xF7 is -9, so 2^9 bytes.
constexpr BYTE sub_cluster_index_block_encoding = 0xF7;

// First file record of the index leaf blocks the sub-cluster fixture and the
// split-extent fixture write.
constexpr DWORD sub_cluster_block_record_base = 110;
constexpr DWORD split_block_record_base = 120;

// A forged $FILE_NAME length that claims more characters than fit.
constexpr BYTE overlong_name_length = 200;

// The boot sector's last two bytes: 0xAA55, little-endian.
constexpr BYTE boot_signature_low = 0xAA;
constexpr BYTE boot_signature_high = 0x55;

// The compressed bytes of the corrupt LZNT1 chunk: a valid chunk header
// (0xB002, little-endian), then a flag byte, and a compressed word whose
// displacement (1) reaches before anything was decompressed.
constexpr BYTE corrupt_chunk_header_low = 0x02;
constexpr BYTE corrupt_chunk_header_high = 0xB0;

// How pattern bytes derive from an index: (i * mul + add) % mod. The modulus
// is prime and not a power of two, so no block size or cluster size repeats
// the pattern.
constexpr unsigned pattern_mul = 31U;
constexpr unsigned pattern_add = 7U;
constexpr unsigned pattern_mod = 251U;

static_assert(attr_offset_value + sizeof(NtfsBrowser::Attr::HeaderNonResident) +
                      run_list_room <
                  offset_of_us_value,
              "attribute data must not reach into the fixup slot");

// The readers bind structs (8-byte aligned at most) onto offsets inside a
// record, so the record itself must start on that boundary: a plain
// std::array<BYTE> may sit at any stack address.
struct alignas(record_alignment) FakeRecord
    : std::array<BYTE, fake_file_record_size> {};

// Packs an on-disk file reference: record number low, sequence number high.
constexpr ULONGLONG MakeFileReference(ULONGLONG record, WORD sequence) {
  return (static_cast<ULONGLONG>(sequence) << NtfsBrowser::mft_sequence_shift) |
         record;
}

// Sequence number a real volume's root directory record carries; non-zero,
// so a reader that forgets to mask it off a parent_ref sees a wrong number.
constexpr WORD root_sequence_number = 5;

// NTFS starts every attribute of a record, and every entry inside one, on an
// 8-byte boundary; the readers bind structs onto these offsets, so an odd one
// is a misaligned access.
constexpr DWORD attr_alignment = 8;

// Returns the sub-node VCN slot: the last 8 bytes of the index entry `e`.
ULONGLONG& SubNodeVcnSlot(NtfsBrowser::Data::IndexEntry& index_entry) {
  const std::span<BYTE> raw(reinterpret_cast<BYTE*>(&index_entry),
                            index_entry.size);
  return *reinterpret_cast<ULONGLONG*>(
      &gsl::at(raw, gsl::narrow<gsl::index>(raw.size() - sizeof(ULONGLONG))));
}

// Rounds a size up to attr_alignment.
constexpr DWORD AlignAttrSize(size_t size) {
  return gsl::narrow<DWORD>((size + attr_alignment - 1) &
                            ~(attr_alignment - 1));
}

// Builds a bare file-record header with the given attribute offset and
// flags.
FakeRecord MakeRecordHeader(WORD offset_of_attr,
                            NtfsBrowser::Flag::FileRecord flags) {
  FakeRecord record{};

  EditFileRecordHeader(record, [&](FileRecordHeader::Data& header) {
    header.magic = file_record_magic;
    header.offset_of_us = offset_of_us_value;
    header.size_of_us = 3;
    header.offset_of_attr = offset_of_attr;
    header.flags = flags;
  });

  return record;
}

// Makes record an extension record: its own sequence number, and the file
// reference of the base record it belongs to.
void SetRecordLink(FakeRecord& record, WORD sequence, ULONGLONG base_ref) {
  EditFileRecordHeader(record, [&](FileRecordHeader::Data& header) {
    header.seq_no = sequence;
    header.ref_to_base = base_ref;
  });
}

// Writes the AttrType::ALL end-of-attributes marker at offset.
void WriteEndOfAttributesMarker(FakeRecord& record, DWORD offset) {
  const auto marker = static_cast<DWORD>(AttrType::All);
  std::memcpy(&record.at(offset), &marker, sizeof(marker));
}

// Encodes text as on-disk UTF-16LE code units: WORD, not wchar_t, which is
// only 16 bits on some platforms (eg. Windows) and 32 on others (eg. Linux).
// A code point above the BMP is split into a surrogate pair; an element
// already in the surrogate range (eg. one half of a pair a 16-bit wchar_t
// already split) passes through as one unit, so this is correct whether
// text arrived pre-split or not.
std::vector<WORD> ToUtf16(std::wstring_view text) {
  constexpr char32_t max_bmp = 0xFFFF;
  constexpr char32_t surrogate_base = 0x10000;
  constexpr unsigned surrogate_shift = 10;
  constexpr char32_t surrogate_mask = 0x3FF;
  constexpr WORD high_surrogate_first = 0xD800;
  constexpr WORD low_surrogate_first = 0xDC00;

  std::vector<WORD> units;
  units.reserve(text.size());
  for (const wchar_t character : text) {
    const auto code_point = static_cast<char32_t>(
        gsl::narrow<std::make_unsigned_t<wchar_t>>(character));
    if (code_point <= max_bmp) {
      units.push_back(static_cast<WORD>(code_point));
      continue;
    }
    const char32_t offset = code_point - surrogate_base;
    units.push_back(
        gsl::narrow<WORD>(high_surrogate_first + (offset >> surrogate_shift)));
    units.push_back(
        gsl::narrow<WORD>(low_surrogate_first + (offset & surrogate_mask)));
  }
  return units;
}

// Writes record at byteOffset, growing image to the next 64 KiB boundary -
// FullCache always reads a whole 64 KiB block, so a short image short-reads.
void PutRecordAt(std::vector<BYTE>& image, size_t byte_offset,
                 const FakeRecord& record) {
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t needed_end = byte_offset + record.size();
  const size_t aligned_end = ((needed_end + full_cache_read_block_size - 1) /
                              full_cache_read_block_size) *
                             full_cache_read_block_size;
  if (image.size() < aligned_end) {
    image.resize(aligned_end, 0);
  }
  std::memcpy(&image.at(byte_offset), record.data(), record.size());
}

// Writes record at $MFT index idx - a plain contiguous slot, mftAddr plus
// idx file records - growing image first if needed.
void PutMftRecord(std::vector<BYTE>& image, DWORD mft_addr, ULONGLONG idx,
                  const FakeRecord& record) {
  PutRecordAt(image,
              mft_addr + static_cast<size_t>(fake_file_record_size) * idx,
              record);
}

// Builds a fake $MFT record: one non-resident DATA attribute whose
// real_size reports sentinel_record_count fake records, with an empty
// data run (GetRecordsCount() only reads real_size).
FakeRecord MakeMftRecord() {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = 0;
  attr.last_vcn = 0;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = sentinel_record_count * fake_file_record_size;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + run_list_room);

  record.at(attr_offset_value + sizeof(attr)) = 0x00;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a fake $MFT record whose DATA attribute has a real, non-empty
// data run of clusters clusters starting at lcn, unlike MakeMftRecord()'s
// empty one.
FakeRecord MakeMftRecordWithRealDataRun(DWORD lcn, DWORD clusters) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = 0;
  attr.last_vcn = clusters - 1;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = ULONGLONG{clusters} * cluster_size;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(clusters);
  std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
  run_len += sizeof(lcn);
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  attr.header.total_size = AlignAttrSize(sizeof(attr) + run_len);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds $MFT's base record: a resident $ATTRIBUTE_LIST with one entry per
// (extension record index, start VCN) pair, then $MFT's own DATA attribute,
// whose last VCN is baseLastVcn. Every entry carries entrySequence as the
// sequence number of the extension record it names.
FakeRecord MakeMftRecordWithDataContinuations(
    std::span<const std::pair<ULONGLONG, ULONGLONG>> continuations,
    ULONGLONG base_last_vcn = 0, WORD entry_sequence = 0) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  DWORD offset = attr_offset_value;

  const auto entry_size =
      AlignAttrSize(NtfsBrowser::Attr::attribute_list_entry_header_size);

  auto& list_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  list_attr.header.type = AttrType::AttributeList;
  list_attr.header.non_resident = 0;
  list_attr.header.name_length = 0;
  list_attr.header.flags = 0;
  list_attr.header.id = 0;
  list_attr.attr_size = entry_size * gsl::narrow<DWORD>(continuations.size());
  list_attr.attr_offset = static_cast<WORD>(sizeof(list_attr));
  list_attr.header.total_size =
      AlignAttrSize(sizeof(list_attr) + list_attr.attr_size);

  for (size_t i = 0; i < continuations.size(); i++) {
    auto& al_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
        &record.at(offset + list_attr.attr_offset + i * entry_size));
    al_entry.attr_type = AttrType::Data;
    al_entry.record_size = gsl::narrow<WORD>(entry_size);
    al_entry.name_length = 0;
    al_entry.name_offset = 0;
    al_entry.start_vcn =
        gsl::at(continuations, gsl::narrow<gsl::index>(i)).second;
    al_entry.base_ref.segment_number =
        gsl::at(continuations, gsl::narrow<gsl::index>(i)).first;
    al_entry.base_ref.sequence_number = entry_sequence;
    al_entry.attr_id = 0;
  }

  offset += list_attr.header.total_size;

  auto& data_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  data_attr.header.type = AttrType::Data;
  data_attr.header.non_resident = 1;
  data_attr.header.name_length = 0;
  data_attr.header.flags = 0;
  data_attr.header.id = 0;
  data_attr.start_vcn = 0;
  data_attr.last_vcn = base_last_vcn;
  data_attr.data_run_offset = static_cast<WORD>(sizeof(data_attr));
  data_attr.comp_unit_size = 0;
  data_attr.real_size = fake_file_record_size;
  data_attr.alloc_size = data_attr.real_size;
  data_attr.ini_size = data_attr.real_size;
  data_attr.header.total_size =
      AlignAttrSize(sizeof(data_attr) + run_list_room);

  record.at(offset + sizeof(data_attr)) = 0x00;

  WriteEndOfAttributesMarker(record, offset + data_attr.header.total_size);
  return record;
}

// Extension record holding the continuation instance of $MFT's own DATA
// attribute: a single non-resident run of clusters clusters at LCN lcn,
// starting at VCN startVcn - unlike MakeMftRecordWithRealDataRun()'s, which
// always starts at VCN 0. It belongs to $MFT (base record 0) and carries
// sequence number sequence.
FakeRecord MakeMftDataContinuationExtensionRecord(ULONGLONG start_vcn,
                                                  DWORD lcn, DWORD clusters,
                                                  WORD sequence = 0) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  SetRecordLink(record, sequence, static_cast<ULONGLONG>(MftIdx::Mft));

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = start_vcn;
  attr.last_vcn = start_vcn + clusters - 1;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = (start_vcn + clusters) * cluster_size;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(clusters);
  std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
  run_len += sizeof(lcn);
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  attr.header.total_size = AlignAttrSize(sizeof(attr) + run_len);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a record with valid magic but offset_of_us == fake_file_record_size,
// which FileRecordHeader's ctor rejects outright.
FakeRecord MakeInvalidOffsetOfUsRecord() {
  FakeRecord record{};

  EditFileRecordHeader(record, [](FileRecordHeader::Data& header) {
    header.magic = file_record_magic;
    header.offset_of_us = fake_file_record_size;
    header.size_of_us = 3;
  });

  return record;
}

// Builds a fake $Volume record whose VOLUME_INFORMATION attribute declares
// attrSize bytes, reporting NTFS 3.1.
FakeRecord MakeVolumeRecordSized(WORD attr_size) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::VolumeInformation;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = attr_size;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& vol_info = *reinterpret_cast<NtfsBrowser::Attr::VolumeInformation*>(
      &record.at(attr_offset_value + attr.attr_offset));
  vol_info.major_version = 3;
  vol_info.minor_version = 1;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

FakeRecord MakeVolumeRecord() {
  return MakeVolumeRecordSized(
      static_cast<WORD>(sizeof(NtfsBrowser::Attr::VolumeInformation)));
}

// Builds a fake $Volume record carrying both VOLUME_INFORMATION (so the
// volume reports NTFS 3.1) and a VOLUME_NAME holding name.
FakeRecord MakeVolumeRecordWithName(std::wstring_view name) {
  FakeRecord record = MakeVolumeRecord();

  const auto& vol_info = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  const DWORD name_offset = attr_offset_value + vol_info.header.total_size;

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(name_offset));
  attr.header.type = AttrType::VolumeName;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 1;
  const std::vector<WORD> encoded_name = ToUtf16(name);
  attr.attr_size = gsl::narrow<DWORD>(encoded_name.size() * sizeof(WORD));
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  std::memcpy(&record.at(name_offset + attr.attr_offset), encoded_name.data(),
              attr.attr_size);

  WriteEndOfAttributesMarker(record, name_offset + attr.header.total_size);
  return record;
}

// Builds a resident $STANDARD_INFORMATION attribute exactly attrSize bytes
// long, so a fixture can pin it to NTFS 1.2's real minimum size.
FakeRecord MakeStandardInformationRecordSized(WORD attr_size) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::StandardInformation;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = attr_size;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& std_info = *reinterpret_cast<NtfsBrowser::Attr::StandardInformation*>(
      &record.at(attr_offset_value + attr.attr_offset));
  std_info.create_time = std_info_create_time;
  std_info.alter_time = std_info_alter_time;
  std_info.mft_time = std_info_mft_time;
  std_info.read_time = std_info_read_time;
  std_info.permission = NtfsBrowser::Flag::StdInfoPermission::ReadOnly;
  std_info.max_version_no = 0;
  std_info.version_no = 0;
  std_info.class_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a bare root-directory record; ParseFileRecord() never looks at
// attributes, so an empty one is enough to exercise a second read.
FakeRecord MakeRootRecord() {
  return MakeRecordHeader(attr_offset_value,
                          NtfsBrowser::Flag::FileRecord::InUse |
                              NtfsBrowser::Flag::FileRecord::Dir);
}

// Directory whose only attribute is a resident $ATTRIBUTE_LIST relocating
// $INDEX_ROOT to the extension record index_extension_idx, reproducing a
// directory that outgrew its base record (eg. C:\Windows).
FakeRecord MakeAttributeListOnlyDirRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(NtfsBrowser::Attr::attribute_list_entry_header_size);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& al_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record.at(attr_offset_value + attr.attr_offset));
  al_entry.attr_type = AttrType::IndexRoot;
  al_entry.record_size =
      static_cast<WORD>(NtfsBrowser::Attr::attribute_list_entry_header_size);
  al_entry.name_length = 0;
  al_entry.name_offset = 0;
  al_entry.start_vcn = 0;
  al_entry.base_ref.segment_number = index_extension_idx;
  al_entry.base_ref.sequence_number = 0;
  al_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Extension record holding the resident $INDEX_ROOT that
// attribute_list_dir_idx's $ATTRIBUTE_LIST points to: a single "Foo" entry
// (file reference 20), plus the terminating nameless entry. baseIdx is the
// record it extends; 0 leaves it a base record.
FakeRecord MakeIndexRootExtensionRecord(ULONGLONG base_idx = 0) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  SetRecordLink(record, 0, base_idx);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::IndexRoot;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: "Foo", a regular (non-directory) file, reference 20.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = first_entry_record;
  first_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  filename.parent_ref = attribute_list_dir_idx;
  filename.flags = NtfsBrowser::Flag::Filename::None;
  filename.name_length = 3;
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  constexpr std::wstring_view foo_name = L"Foo";
  for (BYTE i = 0; i < filename.name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(foo_name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[3]) -
                        reinterpret_cast<BYTE*>(&filename));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size = static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
                   first_entry.size + second_entry.size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Directory whose $ATTRIBUTE_LIST has two entries naming the same
// extension record, once for $INDEX_ROOT and once for $INDEX_ALLOCATION.
FakeRecord MakeAttributeListTwoTypesDirRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  constexpr WORD entry_size_value = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Attr::attribute_list_entry_header_size));

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(entry_size_value) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);

  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(body.data());
  first_entry.attr_type = AttrType::IndexRoot;
  first_entry.record_size = entry_size_value;
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = multi_type_extension_idx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  auto& second_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &gsl::at(body, entry_size_value));
  second_entry.attr_type = AttrType::IndexAllocation;
  second_entry.record_size = entry_size_value;
  second_entry.name_length = 0;
  second_entry.name_offset = 0;
  second_entry.start_vcn = 0;
  second_entry.base_ref.segment_number = multi_type_extension_idx;
  second_entry.base_ref.sequence_number = 0;
  second_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Extension record holding both a resident $INDEX_ROOT (the same single
// "Foo" entry as MakeIndexRootExtensionRecord()) and a minimal
// non-resident $INDEX_ALLOCATION right after it. It extends record baseIdx.
FakeRecord MakeIndexRootAndAllocExtensionRecord(ULONGLONG base_idx) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  SetRecordLink(record, 0, base_idx);

  auto& root_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body = std::span<BYTE>(record).subspan(
      attr_offset_value + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: "Foo", a regular (non-directory) file, reference 20.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = first_entry_record;
  first_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  filename.parent_ref = attr_list_multi_type_dir_idx;
  filename.flags = NtfsBrowser::Flag::Filename::None;
  filename.name_length = 3;
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  constexpr std::wstring_view foo_name = L"Foo";
  for (BYTE i = 0; i < filename.name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(foo_name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[3]) -
                        reinterpret_cast<BYTE*>(&filename));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size + second_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  const DWORD alloc_attr_offset =
      attr_offset_value + root_attr.header.total_size;
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(alloc_attr_offset));
  alloc_attr.header.type = AttrType::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = 0;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = 0;
  alloc_attr.alloc_size = 0;
  alloc_attr.ini_size = 0;
  alloc_attr.header.total_size =
      AlignAttrSize(sizeof(alloc_attr) + run_list_room);

  // Data run: a single 0x00 byte terminates the run list immediately -
  // nothing reads through it in this fixture.
  record.at(alloc_attr_offset + sizeof(alloc_attr)) = 0x00;

  WriteEndOfAttributesMarker(record,
                             alloc_attr_offset + alloc_attr.header.total_size);
  return record;
}

FakeRecord MakeUndersizedResidentAttrRecord() {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  constexpr DWORD undersized_total_size = 17;
  static_assert(undersized_total_size <
                    sizeof(NtfsBrowser::Attr::HeaderResident),
                "total_size must be smaller than a resident attribute header");

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::ReparsePoint;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.header.total_size = undersized_total_size;

  record.at(attr_offset_value + sizeof(NtfsBrowser::Attr::HeaderResident)) =
      forged_byte;

  return record;
}

// LCN for the forged index block, placed past every record's cluster range.
constexpr DWORD forged_index_block_lcn = 20;

// LCN where BuildFakeNtfsImageWithGapCollationSubNode() writes its own
// index block, past every record's cluster range.
constexpr DWORD gap_collation_index_block_lcn = 20;

// Builds a directory record whose $INDEX_ALLOCATION references a single
// forged index block, reached through TraverseSubNode() without real
// B+-tree comparisons.
FakeRecord MakeIndexAllocDirRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  DWORD offset = attr_offset_value;

  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = forged_index_block_size;
  root.clusters_per_ib =
      static_cast<BYTE>(forged_index_block_size / cluster_size);
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode |
                      NtfsBrowser::Flag::IndexEntry::Last;
  // Size covers the header up to stream, plus the 8-byte subnode VCN.
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = AttrType::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn =
      static_cast<ULONGLONG>(forged_index_block_size / cluster_size) - 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = forged_index_block_size;
  alloc_attr.alloc_size = forged_index_block_size;
  alloc_attr.ini_size = forged_index_block_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = 4-byte LCN offset field; low nibble = 1-byte run length.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) =
      static_cast<BYTE>(forged_index_block_size / cluster_size);
  {
    const DWORD lcn = forged_index_block_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);

  offset += alloc_attr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Extension record: a minimal non-resident $INDEX_ALLOCATION whose
// real_size is the given sentinel. It extends record baseIdx.
FakeRecord MakeIndexAllocationOnlyExtensionRecord(DWORD real_size,
                                                  ULONGLONG base_idx) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  SetRecordLink(record, 0, base_idx);

  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(attr_offset_value));
  alloc_attr.header.type = AttrType::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = 0;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = real_size;
  alloc_attr.alloc_size = real_size;
  alloc_attr.ini_size = real_size;
  alloc_attr.header.total_size =
      AlignAttrSize(sizeof(alloc_attr) + run_list_room);

  // Data run: a single 0x00 byte terminates the run list immediately.
  record.at(attr_offset_value + sizeof(alloc_attr)) = 0x00;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + alloc_attr.header.total_size);
  return record;
}

// Directory whose $ATTRIBUTE_LIST has four entries, each naming a
// different extension record for the same attribute type.
FakeRecord MakeFragmentedAttributeListDirRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  constexpr WORD entry_size_value = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Attr::attribute_list_entry_header_size));

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(entry_size_value) * 4;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::array<ULONGLONG, 4> extension_idxs{
      uaf_extension_idx0, uaf_extension_idx1, uaf_extension_idx2,
      uaf_extension_idx3};

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);
  for (size_t i = 0; i < extension_idxs.size(); i++) {
    auto& entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
        &gsl::at(body, gsl::narrow<gsl::index>(i * entry_size_value)));
    entry.attr_type = AttrType::IndexAllocation;
    entry.record_size = entry_size_value;
    entry.name_length = 0;
    entry.name_offset = 0;
    entry.start_vcn = 0;
    entry.base_ref.segment_number = extension_idxs.at(i);
    entry.base_ref.sequence_number = 0;
    entry.attr_id = 0;
  }

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// A single resident $DATA attribute whose name_offset/name_length point
// past its own declared total_size, while still landing on known,
// deterministic bytes inside the record buffer.
FakeRecord MakeAttrNameExceedsTotalSizeRecord() {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  constexpr DWORD body_size = 4;

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = body_size;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + body_size);

  static_assert(static_cast<DWORD>(attr_name_bounds_name_offset) +
                        2 * static_cast<DWORD>(attr_name_bounds_name_length) >
                    sizeof(NtfsBrowser::Attr::HeaderResident) + body_size,
                "name must exceed total_size");
  static_assert(attr_name_bounds_sentinel.size() ==
                    static_cast<size_t>(attr_name_bounds_name_length),
                "sentinel length must match name_length exactly");

  attr.header.name_length = attr_name_bounds_name_length;
  attr.header.name_offset = attr_name_bounds_name_offset;

  // Past total_size (28), but still inside the 1024-byte record buffer.
  const std::vector<WORD> encoded_sentinel = ToUtf16(attr_name_bounds_sentinel);
  std::memcpy(&record.at(attr_offset_value + attr_name_bounds_name_offset),
              encoded_sentinel.data(),
              static_cast<size_t>(attr_name_bounds_name_length) * sizeof(WORD));

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a root-directory replacement holding a single, well-formed
// resident $DATA attribute whose body is exactly small_resident_data_content.
FakeRecord MakeSmallResidentDataRecord() {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = gsl::narrow<DWORD>(small_resident_data_content.size());
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  std::memcpy(&record.at(attr_offset_value + attr.attr_offset),
              small_resident_data_content.data(),
              small_resident_data_content.size());

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a root-directory replacement whose resident $ATTRIBUTE_LIST holds
// one full, self-referencing entry, followed by a truncated partial one.
FakeRecord MakeAttributeListShortReadRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  constexpr DWORD body_size =
      static_cast<DWORD>(NtfsBrowser::Attr::attribute_list_entry_header_size) +
      10;
  static_assert(
      body_size % NtfsBrowser::Attr::attribute_list_entry_header_size != 0,
      "body size must not be an exact multiple of the entry size, "
      "to reproduce a short final ReadData()");

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = body_size;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record.at(attr_offset_value + attr.attr_offset));
  first_entry.attr_type = AttrType::Data;
  first_entry.record_size =
      static_cast<WORD>(NtfsBrowser::Attr::attribute_list_entry_header_size);
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = static_cast<ULONGLONG>(MftIdx::Root);
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST holds a real
// entry (relocating $INDEX_ROOT to index_extension_idx), followed by one
// whose own record_size is nonzero but smaller than the entry header
// itself. VG4(a): the mid-loop record_size bounds check, never a short
// ReadData().
FakeRecord MakeAttributeListRecordSizeTooSmallDirRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  constexpr WORD entry_size_value = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Attr::attribute_list_entry_header_size));
  // Nonzero, but smaller than entry_size - the exact condition under test.
  constexpr WORD too_small_record_size = 5;

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(entry_size_value) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);

  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(body.data());
  first_entry.attr_type = AttrType::IndexRoot;
  first_entry.record_size = entry_size_value;
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = index_extension_idx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  // Relocates nowhere - base_ref names this same directory record, so
  // AttrList skips it - only its record_size matters here.
  auto& second_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &gsl::at(body, entry_size_value));
  second_entry.attr_type = AttrType::Data;
  second_entry.record_size = too_small_record_size;
  second_entry.name_length = 0;
  second_entry.name_offset = 0;
  second_entry.start_vcn = 0;
  second_entry.base_ref.segment_number = attribute_list_dir_idx;
  second_entry.base_ref.sequence_number = 0;
  second_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST holds a single
// real entry (relocating $INDEX_ROOT to index_extension_idx) whose own
// record_size overshoots the attribute's declared size. VG4(b): the
// post-loop offset-vs-size check, reached through AttrList's normal
// (nullopt) end, not a short ReadData().
FakeRecord MakeAttributeListOffsetMismatchDirRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  constexpr WORD entry_size_value = static_cast<WORD>(
      AlignAttrSize(NtfsBrowser::Attr::attribute_list_entry_header_size));
  // Past entry_size, so the single entry's declared span overshoots the
  // attribute's own declared size below.
  constexpr WORD overshoot_record_size =
      static_cast<WORD>(entry_size_value + 4);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(entry_size_value);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record.at(attr_offset_value + attr.attr_offset));
  first_entry.attr_type = AttrType::IndexRoot;
  first_entry.record_size = overshoot_record_size;
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = index_extension_idx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST, via a single
// full entry, relocates to targetIdx.
FakeRecord MakeAttributeListCycleRecord(ULONGLONG target_idx) {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(NtfsBrowser::Attr::attribute_list_entry_header_size);
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(
      &record.at(attr_offset_value + attr.attr_offset));
  first_entry.attr_type = AttrType::AttributeList;
  first_entry.record_size =
      static_cast<WORD>(NtfsBrowser::Attr::attribute_list_entry_header_size);
  first_entry.name_length = 0;
  first_entry.name_offset = 0;
  first_entry.start_vcn = 0;
  first_entry.base_ref.segment_number = target_idx;
  first_entry.base_ref.sequence_number = 0;
  first_entry.attr_id = 0;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $ATTRIBUTE_LIST packs two
// entries at the real on-disk stride (attribute_list_real_entry_size), not
// sizeof(Attr::AttributeList).
FakeRecord MakeAttributeListTightlyPackedDirRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size = static_cast<DWORD>(attribute_list_real_entry_size) * 2;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);

  // The second entry sits at an odd stride on purpose, so it cannot be bound
  // to a struct reference: fill an aligned copy and copy the on-disk bytes.
  const auto write_entry = [](BYTE* dest, AttrType type, ULONGLONG record_idx,
                              WORD attr_id) {
    NtfsBrowser::Attr::AttributeList entry{};
    entry.attr_type = type;
    entry.record_size = attribute_list_real_entry_size;
    entry.name_length = 0;
    entry.name_offset = 0;
    entry.start_vcn = 0;
    entry.base_ref.segment_number = record_idx;
    entry.base_ref.sequence_number = 0;
    entry.attr_id = attr_id;
    std::memcpy(dest, &entry, attribute_list_real_entry_size);
  };
  write_entry(body.data(), AttrType::IndexRoot, attr_list_tight_pack_ext_idx_a,
              0);
  write_entry(&gsl::at(body, attribute_list_real_entry_size),
              AttrType::IndexAllocation, attr_list_tight_pack_ext_idx_b, 1);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a root-directory replacement whose sole attribute is a resident,
// named $DATA stream (an ADS) holding named_data_stream_content under
// named_data_stream_name.
FakeRecord MakeNamedDataStreamRecord() {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  const std::vector<WORD> encoded_name = ToUtf16(named_data_stream_name);
  attr.header.name_length = named_data_stream_name_length;
  attr.header.name_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = gsl::narrow<DWORD>(named_data_stream_content.size());
  attr.attr_offset =
      gsl::narrow<WORD>(sizeof(attr) + encoded_name.size() * sizeof(WORD));
  attr.header.total_size = AlignAttrSize(attr.attr_offset + attr.attr_size);

  std::memcpy(&record.at(attr_offset_value + attr.header.name_offset),
              encoded_name.data(), encoded_name.size() * sizeof(WORD));
  std::memcpy(&record.at(attr_offset_value + attr.attr_offset),
              named_data_stream_content.data(),
              named_data_stream_content.size());

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a directory record whose resident $INDEX_ROOT holds one real
// FILE_NAME entry (name, mftIndex, parentRef) plus the terminating entry.
FakeRecord MakeIndexRootDirRecord(std::wstring_view name, ULONGLONG mft_index,
                                  ULONGLONG parent_ref) {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::IndexRoot;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: the single real FILE_NAME entry this variant declares.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = mft_index;
  first_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  filename.parent_ref = parent_ref;
  filename.flags = NtfsBrowser::Flag::Filename::None;
  filename.name_length = gsl::narrow<BYTE>(name.size());
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  for (BYTE i = 0; i < filename.name_length; i++) {
    // name_length is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size = gsl::narrow<WORD>(
      reinterpret_cast<BYTE*>(&filename.name[filename.name_length]) -
      reinterpret_cast<BYTE*>(&filename));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size = static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
                   first_entry.size + second_entry.size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a root-directory replacement whose own $INDEX_ROOT holds one
// real, non-terminal FILE_NAME entry that is also a sub-node pointer into
// a real $INDEX_ALLOCATION index block.
FakeRecord MakeRootRecordWithGapCollationSubNode() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  DWORD offset = attr_offset_value;

  // $INDEX_ROOT
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1 ("A_"): non-terminal, so it carries both a name and a sub-node
  // VCN right after it.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = gap_collation_non_terminal_mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = static_cast<ULONGLONG>(MftIdx::Root);
  fn1.flags = NtfsBrowser::Flag::Filename::Directory;
  constexpr std::wstring_view non_terminal_name = L"A_";
  fn1.name_length = 2;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  for (BYTE i = 0; i < fn1.name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(non_terminal_name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  root.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size + second_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  // $INDEX_ALLOCATION
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = AttrType::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = 0;  // single cluster -> VCN 0 only
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = cluster_size;
  alloc_attr.alloc_size = cluster_size;
  alloc_attr.ini_size = cluster_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = 1;  // 1 cluster
  {
    const DWORD lcn = gap_collation_index_block_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);

  offset += alloc_attr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// LCN where BuildFakeNtfsImageWithDeepIndexBlockChain() writes its chained
// index blocks, kept clear of every other fixture's placement in this file.
constexpr DWORD index_block_chain_lcn = 100;

// Builds a root-directory replacement whose $INDEX_ROOT sub-node pointer
// leads into a index_block_chain_length-block chained $INDEX_ALLOCATION.
FakeRecord MakeIndexBlockChainRootRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  DWORD offset = attr_offset_value;

  // $INDEX_ROOT
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = cluster_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode |
                      NtfsBrowser::Flag::IndexEntry::Last;
  // Header plus the 8-byte subnode VCN that replaces "stream" when empty.
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  // $INDEX_ALLOCATION: one data run, index_block_chain_length clusters starting
  // at index_block_chain_lcn.
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = AttrType::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = index_block_chain_length - 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = ULONGLONG{index_block_chain_length} * cluster_size;
  alloc_attr.alloc_size = alloc_attr.real_size;
  alloc_attr.ini_size = alloc_attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // Data run header byte: high nibble = LCN offset field size (4 bytes),
  // low nibble = length field size (1 byte) - standard NTFS run encoding
  // (AttrNonResident::PickData, src/attr-non-resident.cpp).
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = static_cast<BYTE>(index_block_chain_length);
  {
    const DWORD lcn = index_block_chain_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);

  offset += alloc_attr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// LCN where BuildFakeNtfsImageWithOrphanedIndexBlocks() writes its three
// index blocks, kept clear of every other fixture's placement in this file.
constexpr DWORD orphaned_blocks_lcn = 200;

// Number of index blocks (VCN 0-2) the fixture's $INDEX_ALLOCATION covers.
constexpr DWORD orphaned_blocks_count = 3;

// Builds a root-directory replacement whose $INDEX_ROOT points only at
// VCN 0, while its $INDEX_ALLOCATION stream is sized for
// orphaned_blocks_count blocks - VCN 1 and 2 exist on "disk" but no pointer
// in the tree reaches them. declaredBlockCount, when different from
// orphaned_blocks_count, forges the attribute's own real_size (hence
// GetIndexBlockCount()) without changing the data run: blocks beyond
// orphaned_blocks_count are then declared but never actually backed.
FakeRecord MakeOrphanedIndexBlocksRootRecord(
    ULONGLONG declared_block_count = orphaned_blocks_count) {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);
  // The sequence its entries' parent references carry.
  EditFileRecordHeader(record, [](FileRecordHeader::Data& header) {
    header.seq_no = root_sequence_number;
  });

  DWORD offset = attr_offset_value;

  // $INDEX_ROOT
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = cluster_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Sole entry: nameless, terminal, pointing at VCN 0 - the only block a
  // normal B+ tree walk reaches in this fixture.
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode |
                      NtfsBrowser::Flag::IndexEntry::Last;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  // $INDEX_ALLOCATION: orphaned_blocks_count contiguous blocks at
  // orphaned_blocks_lcn, only the first ever pointed at from $INDEX_ROOT.
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = AttrType::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = orphaned_blocks_count - 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = declared_block_count * cluster_size;
  alloc_attr.alloc_size = alloc_attr.real_size;
  alloc_attr.ini_size = alloc_attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = static_cast<BYTE>(orphaned_blocks_count);
  {
    const DWORD lcn = orphaned_blocks_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);

  offset += alloc_attr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes a single-cluster index block at "vcn" (relative to
// orphaned_blocks_lcn) holding one real leaf entry, into "image".
void WriteOrphanedIndexLeafBlock(std::vector<BYTE>& image, DWORD vcn,
                                 ULONGLONG mft_ref, ULONGLONG parent_ref,
                                 std::wstring_view name) {
  const auto name_length = gsl::narrow<BYTE>(name.size());
  const size_t blocks_offset =
      static_cast<size_t>(orphaned_blocks_lcn) * cluster_size;
  const std::span<BYTE> block_start = std::span<BYTE>(image).subspan(
      blocks_offset + static_cast<size_t>(vcn) * cluster_size);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points at the block's own last 6 bytes, so PatchUS() succeeds trivially
  // without a real fixup array.
  block.offset_of_us = static_cast<WORD>(cluster_size - us_slot_size);
  block.size_of_us = 3;
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = parent_ref;
  fn1.flags = NtfsBrowser::Flag::Filename::None;
  fn1.name_length = name_length;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  for (BYTE i = 0; i < name_length; i++) {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  block.total_entry_size = first_entry.size;
  block.alloc_entry_size = first_entry.size;
}

// LCN where BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock() writes
// its index blocks, kept clear of every other fixture's placement in this
// file.
constexpr DWORD multi_cluster_orphan_lcn = 210;

// Index block size in bytes for that fixture: more than one cluster, so the
// blockIndex-to-VCN scaling under test actually multiplies.
constexpr DWORD multi_cluster_orphan_index_block_size =
    static_cast<DWORD>(multi_cluster_orphan_clusters_per_block) * cluster_size;

// Two real index blocks: block 0 (VCN 0, reachable) and block 1 (VCN
// multi_cluster_orphan_clusters_per_block, orphaned).
constexpr DWORD multi_cluster_orphan_block_count = 2;

// Builds a root-directory replacement whose $INDEX_ROOT points only at
// block 0, while $INDEX_ALLOCATION covers multi_cluster_orphan_block_count
// blocks of multi_cluster_orphan_clusters_per_block clusters each.
FakeRecord MakeMultiClusterOrphanedIndexBlocksRootRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);
  // The sequence its entries' parent references carry.
  EditFileRecordHeader(record, [](FileRecordHeader::Data& header) {
    header.seq_no = root_sequence_number;
  });

  DWORD offset = attr_offset_value;

  // $INDEX_ROOT: a single sub-node pointer at VCN 0 (block 0).
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = multi_cluster_orphan_index_block_size;
  root.clusters_per_ib = multi_cluster_orphan_clusters_per_block;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode |
                      NtfsBrowser::Flag::IndexEntry::Last;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;  // block 0

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  // $INDEX_ALLOCATION: multi_cluster_orphan_block_count contiguous blocks at
  // multi_cluster_orphan_lcn, only block 0 ever pointed at from $INDEX_ROOT.
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = AttrType::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  const DWORD total_clusters =
      static_cast<DWORD>(multi_cluster_orphan_block_count) *
      multi_cluster_orphan_clusters_per_block;
  alloc_attr.last_vcn = total_clusters - 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size =
      static_cast<ULONGLONG>(multi_cluster_orphan_block_count) *
      multi_cluster_orphan_index_block_size;
  alloc_attr.alloc_size = alloc_attr.real_size;
  alloc_attr.ini_size = alloc_attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(total_clusters);
  {
    const DWORD lcn = multi_cluster_orphan_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);

  offset += alloc_attr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes a real index block at block index "blockIndex" (VCN blockIndex *
// multi_cluster_orphan_clusters_per_block), holding one real leaf entry.
void WriteMultiClusterIndexLeafBlock(std::vector<BYTE>& image,
                                     DWORD block_index, ULONGLONG mft_ref,
                                     ULONGLONG parent_ref,
                                     std::wstring_view name) {
  const auto name_length = gsl::narrow<BYTE>(name.size());
  const ULONGLONG vcn = static_cast<ULONGLONG>(block_index) *
                        multi_cluster_orphan_clusters_per_block;
  const size_t blocks_offset =
      static_cast<size_t>(multi_cluster_orphan_lcn) * cluster_size;
  const std::span<BYTE> block_start = std::span<BYTE>(image).subspan(
      blocks_offset + gsl::narrow<size_t>(vcn) * cluster_size);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points the USA at the block's own last (1 + sectors) words, so every
  // PatchUS() check compares a still-zero byte range to itself and the
  // whole array trivially self-patches, without a real fixup array - the
  // multi-sector generalization of the single-cluster fixtures' same trick.
  const DWORD sectors =
      multi_cluster_orphan_index_block_size / bytes_per_sector;
  block.offset_of_us = gsl::narrow<WORD>(multi_cluster_orphan_index_block_size -
                                         2 * (1 + sectors));
  block.size_of_us = gsl::narrow<WORD>(1 + sectors);
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = parent_ref;
  fn1.flags = NtfsBrowser::Flag::Filename::None;
  fn1.name_length = name_length;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  for (BYTE i = 0; i < name_length; i++) {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  block.total_entry_size = first_entry.size;
  block.alloc_entry_size = first_entry.size;
}

// One $INDEX_ALLOCATION instance built by MakeDirectoryWithIndexAllocation():
// "clusters" clusters at "lcn", from virtual cluster "start_vcn" on.
struct FakeIndexAllocExtent {
  ULONGLONG start_vcn;
  DWORD clusters;
  DWORD lcn;
};

// Builds a root-directory replacement, sequence root_sequence_number, whose
// $INDEX_ROOT points only at block 0. Its $INDEX_ALLOCATION has one instance
// per extent. The first declares declaredBlockCount blocks of ibSize bytes.
// The others declare no size, as a real continuation does.
FakeRecord MakeDirectoryWithIndexAllocation(
    DWORD ib_size, std::span<const FakeIndexAllocExtent> extents,
    ULONGLONG declared_block_count) {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);
  EditFileRecordHeader(record, [](FileRecordHeader::Data& header) {
    header.seq_no = root_sequence_number;
  });

  DWORD offset = attr_offset_value;

  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = ib_size;
  root.clusters_per_ib =
      gsl::narrow<BYTE>(ib_size >= cluster_size ? ib_size / cluster_size : 1);
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode |
                      NtfsBrowser::Flag::IndexEntry::Last;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;  // block 0

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  bool first = true;
  for (const FakeIndexAllocExtent& extent : extents) {
    auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
        &record.at(offset));
    alloc_attr.header.type = AttrType::IndexAllocation;
    alloc_attr.header.non_resident = 1;
    alloc_attr.header.name_length = 0;
    alloc_attr.header.flags = 0;
    alloc_attr.header.id = 0;
    alloc_attr.start_vcn = extent.start_vcn;
    alloc_attr.last_vcn = extent.start_vcn + extent.clusters - 1;
    alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
    alloc_attr.comp_unit_size = 0;
    alloc_attr.real_size = first ? declared_block_count * ib_size : 0;
    alloc_attr.alloc_size = alloc_attr.real_size;
    alloc_attr.ini_size = alloc_attr.real_size;
    first = false;

    const std::span<BYTE> data_run =
        std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
    DWORD run_len = 0;
    // High nibble = LCN offset field size, low nibble = length field size.
    gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
    gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(extent.clusters);
    std::memcpy(&gsl::at(data_run, run_len), &extent.lcn, sizeof(extent.lcn));
    run_len += sizeof(extent.lcn);
    gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

    alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);
    offset += alloc_attr.header.total_size;
  }

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes an index block of blockSize bytes at byte offset blockOffset, holding
// one real leaf entry named "name", filed under parentRef.
void WriteIndexLeafBlockAt(std::vector<BYTE>& image, size_t block_offset,
                           DWORD block_size, ULONGLONG vcn, ULONGLONG mft_ref,
                           ULONGLONG parent_ref, std::wstring_view name) {
  const std::span<BYTE> block_start =
      std::span<BYTE>(image).subspan(block_offset);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points the USA at the block's own last (1 + sectors) words, so PatchUS()
  // compares a still-zero range to itself, without a real fixup array.
  const DWORD sectors = block_size / bytes_per_sector;
  block.offset_of_us = gsl::narrow<WORD>(block_size - 2 * (1 + sectors));
  block.size_of_us = gsl::narrow<WORD>(1 + sectors);
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = parent_ref;
  fn1.flags = NtfsBrowser::Flag::Filename::None;
  fn1.name_length = gsl::narrow<BYTE>(name.size());
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  for (size_t i = 0; i < name.size(); i++) {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[name.size()]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  block.total_entry_size = first_entry.size;
  block.alloc_entry_size = first_entry.size;
}

// Clusters BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks()'s
// $INDEX_ALLOCATION maps: two 512-byte blocks per 1024-byte cluster, so four
// blocks in all.
constexpr DWORD sub_cluster_alloc_clusters = 2;

// Clusters each of BuildFakeNtfsImageWithSplitIndexAllocation()'s two
// $INDEX_ALLOCATION instances maps: one block per cluster, so four blocks in
// all.
constexpr DWORD split_extent_clusters = 2;

////////////////////////////////////////////////////////////////////////////
// NTFS compression fixtures (see fake-ntfs-image.h for what each builds)
////////////////////////////////////////////////////////////////////////////

// AttrHeaderCommon::flags bit 0 ("compressed"); unread by the library itself
// but set here since a real compressed attribute always sets it too.
constexpr WORD attr_flag_compressed = 0x0001;

// Encodes "runs" into NTFS' real, delta-LCN run-list format at "dataRun"
// (terminated by 0x00), and returns the byte count written.
DWORD EncodeDataRuns(std::span<BYTE> data_run,
                     const std::vector<FakeDataRun>& runs) {
  DWORD run_len = 0;
  DWORD previous_lcn = 0;

  for (const FakeDataRun& run : runs) {
    if (run.lcn) {
      gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
      gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(run.clusters);
      const LONG delta =
          gsl::narrow<LONG>(*run.lcn) - gsl::narrow<LONG>(previous_lcn);
      std::memcpy(&gsl::at(data_run, run_len), &delta, sizeof(delta));
      run_len += sizeof(delta);
      previous_lcn = *run.lcn;
    } else {
      gsl::at(data_run, run_len++) = 0x01;
      gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(run.clusters);
    }
  }

  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list
  return run_len;
}

// Writes one resident $STANDARD_INFORMATION attribute at record[offset]
// with the given DOS permission bits, and returns its total_size.
DWORD WriteStandardInformationAttr(
    FakeRecord& record, DWORD offset,
    NtfsBrowser::Flag::StdInfoPermission permission) {
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::StandardInformation;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::StandardInformation));
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& std_info = *reinterpret_cast<NtfsBrowser::Attr::StandardInformation*>(
      &record.at(offset + attr.attr_offset));
  std_info.create_time = std_info_create_time;
  std_info.alter_time = std_info_alter_time;
  std_info.mft_time = std_info_mft_time;
  std_info.read_time = std_info_read_time;
  std_info.permission = permission;

  return attr.header.total_size;
}

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

// Writes one non-resident attribute at record[offset] and returns its
// declared total_size. compUnitSize == 0 is ordinary; non-zero adds the
// trailing 8-byte CompressedSize field.
DWORD WriteNonResidentAttr(FakeRecord& record, DWORD offset, AttrType type,
                           WORD comp_unit_size, ULONGLONG real_size,
                           const std::vector<FakeDataRun>& runs,
                           const FakeNonResidentOverrides& overrides = {}) {
  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  attr.header.type = type;
  attr.header.non_resident = 1;
  const std::vector<WORD> encoded_name = ToUtf16(overrides.name);
  assert(encoded_name.size() <= 255 && "on-disk name_length is one byte");
  attr.header.name_length = gsl::narrow<BYTE>(encoded_name.size());
  attr.header.flags = overrides.flags.value_or(
      (comp_unit_size != 0) ? attr_flag_compressed : static_cast<WORD>(0));
  attr.header.id = 0;

  ULONGLONG total_clusters = 0;
  ULONGLONG real_clusters = 0;
  for (const FakeDataRun& run : runs) {
    total_clusters += run.clusters;
    if (run.lcn) {
      real_clusters += run.clusters;
    }
  }

  attr.start_vcn = overrides.start_vcn;
  attr.last_vcn = overrides.last_vcn.value_or(
      overrides.start_vcn + ((total_clusters == 0) ? 0 : total_clusters - 1));
  attr.comp_unit_size = comp_unit_size;
  attr.alloc_size = total_clusters * cluster_size;
  attr.real_size = real_size;
  attr.ini_size = overrides.ini_size.value_or(real_size);

  const auto header_size = gsl::narrow<WORD>(
      sizeof(attr) + ((comp_unit_size != 0)
                          ? NtfsBrowser::Attr::compressed_size_field_size
                          : 0));
  const size_t name_bytes = encoded_name.size() * sizeof(WORD);
  if (name_bytes != 0) {
    attr.header.name_offset = header_size;
    std::memcpy(&record.at(offset + header_size), encoded_name.data(),
                name_bytes);
  }
  const auto run_offset = gsl::narrow<WORD>(header_size + name_bytes);
  attr.data_run_offset = run_offset;

  if (comp_unit_size != 0) {
    // CompressedSize: total allocated size of the attribute's compressed
    // clusters, i.e. everything actually on disk (the sparse padding of each
    // compressed unit excluded).
    const ULONGLONG compressed_size = real_clusters * cluster_size;
    std::memcpy(&record.at(offset + sizeof(attr)), &compressed_size,
                sizeof(compressed_size));
  }

  DWORD run_len = 0;
  if (overrides.raw_runs.empty()) {
    run_len = EncodeDataRuns(
        std::span<BYTE>(record).subspan(offset + run_offset), runs);
  } else {
    run_len = gsl::narrow<DWORD>(overrides.raw_runs.size());
    std::memcpy(&record.at(offset + run_offset), overrides.raw_runs.data(),
                run_len);
  }
  attr.header.total_size =
      overrides.total_size.value_or(AlignAttrSize(run_offset + run_len));
  return attr.header.total_size;
}

// File record (root, #5): resident $STANDARD_INFORMATION("permission") plus
// one non-resident $DATA attribute described by compUnitSize/realSize/runs.
FakeRecord
    MakeNonResidentDataRecord(NtfsBrowser::Flag::StdInfoPermission permission,
                              WORD comp_unit_size, ULONGLONG real_size,
                              const std::vector<FakeDataRun>& runs,
                              const FakeNonResidentOverrides& overrides = {}) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(record, offset, permission);
  offset += WriteNonResidentAttr(record, offset, AttrType::Data, comp_unit_size,
                                 real_size, runs, overrides);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// One leaf $FILE_NAME index entry a fixture writes: a name, the MFT record
// it points at, and whether that record is a directory.
struct FakeIndexName {
  std::wstring_view name;
  ULONGLONG mft_ref;
  bool directory;
};

// Writes "name" as a leaf index entry at "dest" and returns its size in
// bytes. Entries are packed with no padding, like every other fixture here.
WORD WriteFilenameEntry(BYTE* dest, const FakeIndexName& name) {
  auto& entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(dest);
  entry.mft_index = name.mft_ref;
  entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&entry.stream);
  filename.parent_ref = static_cast<ULONGLONG>(MftIdx::Root);
  filename.flags = name.directory ? NtfsBrowser::Flag::Filename::Directory
                                  : NtfsBrowser::Flag::Filename::None;
  const std::vector<WORD> encoded_name = ToUtf16(name.name);
  filename.name_length = gsl::narrow<BYTE>(encoded_name.size());
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  for (size_t i = 0; i < encoded_name.size(); i++) {
    filename.name[i] = encoded_name.at(i);
  }

  entry.stream_size = gsl::narrow<WORD>(
      reinterpret_cast<BYTE*>(&filename.name[filename.name_length]) -
      reinterpret_cast<BYTE*>(&filename));
  entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &entry.stream - reinterpret_cast<BYTE*>(&entry) + entry.stream_size));
  return entry.size;
}

// Directory record (root, #5): resident $STANDARD_INFORMATION("permission"),
// a resident $INDEX_ROOT holding "rootNames" as leaf entries followed by a
// nameless SUBNODE-only entry, and a non-resident $INDEX_ALLOCATION - lets
// fixtures target an attribute FuzzOnce() actually ReadData()s, unlike plain
// $DATA.
FakeRecord MakeIndexAllocationDirRecord(
    NtfsBrowser::Flag::StdInfoPermission permission, WORD comp_unit_size,
    ULONGLONG real_size, const std::vector<FakeDataRun>& runs,
    const FakeNonResidentOverrides& overrides = {},
    std::span<const FakeIndexName> root_names = {}) {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(record, offset, permission);

  // $INDEX_ROOT
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  const std::span<BYTE> entries =
      body.subspan(sizeof(NtfsBrowser::Attr::IndexRoot));
  DWORD leaf_bytes = 0;
  for (const FakeIndexName& name : root_names) {
    leaf_bytes += WriteFilenameEntry(&gsl::at(entries, leaf_bytes), name);
  }

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(entries, leaf_bytes));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode |
                      NtfsBrowser::Flag::IndexEntry::Last;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  root.total_entry_size = leaf_bytes + first_entry.size;
  root.alloc_entry_size = leaf_bytes + first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) + leaf_bytes +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;
  offset += WriteNonResidentAttr(record, offset, AttrType::IndexAllocation,
                                 comp_unit_size, real_size, runs, overrides);

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Directory record (root, #5): a resident $INDEX_ROOT holding "names" as leaf
// entries, then the terminating entry.
FakeRecord
    MakeIndexRootDirRecordWithNames(std::span<const FakeIndexName> names) {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::IndexRoot;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(attr_offset_value + attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  const std::span<BYTE> entries =
      body.subspan(sizeof(NtfsBrowser::Attr::IndexRoot));
  DWORD leaf_bytes = 0;
  for (const FakeIndexName& name : names) {
    leaf_bytes += WriteFilenameEntry(&gsl::at(entries, leaf_bytes), name);
  }

  auto& last = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(entries, leaf_bytes));
  last.flags = NtfsBrowser::Flag::IndexEntry::Last;
  last.stream_size = 0;
  last.size = gsl::narrow<WORD>(
      AlignAttrSize(&last.stream - reinterpret_cast<BYTE*>(&last)));

  root.total_entry_size = leaf_bytes + last.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  attr.attr_size = static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
                   leaf_bytes + last.size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Lays "clusterBytes" down over the real runs of "runs", in run order, growing
// "image" to hold them; sparse runs consume no bytes and stay zero-filled.
void LayRunBytes(std::vector<BYTE>& image, const std::vector<FakeDataRun>& runs,
                 const std::vector<BYTE>& cluster_bytes) {
  // Grow the image so every real run fits, rounded up to FullCache's whole
  // 64KiB read block - same reasoning as
  // BuildFakeNtfsImageWithDeepIndexBlockChain().
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  size_t highest_end = image.size();
  for (const FakeDataRun& run : runs) {
    if (run.lcn) {
      const size_t end = (static_cast<size_t>(*run.lcn) + run.clusters) *
                         static_cast<size_t>(cluster_size);
      highest_end = (end > highest_end) ? end : highest_end;
    }
  }
  const size_t aligned_end = ((highest_end + full_cache_read_block_size - 1) /
                              full_cache_read_block_size) *
                             full_cache_read_block_size;
  if (image.size() < aligned_end) {
    image.resize(aligned_end, 0);
  }

  size_t written = 0;
  for (const FakeDataRun& run : runs) {
    if (!run.lcn || written >= cluster_bytes.size()) {
      continue;
    }

    const size_t capacity =
        static_cast<size_t>(run.clusters) * static_cast<size_t>(cluster_size);
    const size_t left = cluster_bytes.size() - written;
    const size_t chunk = (left < capacity) ? left : capacity;
    std::memcpy(&image.at(static_cast<size_t>(*run.lcn) *
                          static_cast<size_t>(cluster_size)),
                &cluster_bytes.at(written), chunk);
    written += chunk;
  }
}

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// "record" and "clusterBytes" laid down over its real runs, in run order;
// sparse runs consume no bytes and stay zero-filled.
std::vector<BYTE>
    BuildCompressionImage(const FakeRecord& record,
                          const std::vector<FakeDataRun>& runs,
                          const std::vector<BYTE>& cluster_bytes) {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  LayRunBytes(image, runs, cluster_bytes);
  return image;
}

// The malformed LZNT1 bytes both corrupt-compression fixtures use: a
// well-formed compressed chunk header, then a compressed word whose
// displacement (1) reaches before anything has been decompressed yet.
std::vector<BYTE> MakeCorruptLznt1Chunk() {
  return {corrupt_chunk_header_low, corrupt_chunk_header_high, 0x01, 0x00,
          0x00};
}

// The 1024-byte "INDX"-signed index block a compressed $INDEX_ALLOCATION
// decompresses to: "names" as leaf FILE_NAME entries plus the terminating
// entry. Built standalone since it is the *decompressed* content, wrapped
// into an LZNT1 chunk by the caller.
std::vector<BYTE> MakeIndexBlockContent(std::span<const FakeIndexName> names) {
  std::vector<BYTE> content(fake_file_record_size, 0);

  const std::span<BYTE> block_start = content;
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  block.magic = index_block_magic;
  // Points offset_of_us at the fixup slot itself, so PatchUS() trivially
  // succeeds - same technique as BuildFakeNtfsImageWithGapCollationSubNode().
  block.offset_of_us = static_cast<WORD>(fake_file_record_size - us_slot_size);
  block.size_of_us = 3;
  block.vcn = 0;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));

  DWORD leaf_bytes = 0;
  for (const FakeIndexName& name : names) {
    leaf_bytes += WriteFilenameEntry(&gsl::at(body, leaf_bytes), name);
  }

  auto& last = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, leaf_bytes));
  last.flags = NtfsBrowser::Flag::IndexEntry::Last;
  last.stream_size = 0;
  last.size = gsl::narrow<WORD>(
      AlignAttrSize(&last.stream - reinterpret_cast<BYTE*>(&last)));

  block.total_entry_size = leaf_bytes + last.size;
  block.alloc_entry_size = block.total_entry_size;

  return content;
}

// The single "Comp" entry every compressed $INDEX_ALLOCATION fixture but
// the surrogate-pair one decompresses to.
std::vector<BYTE> MakeCompressedIndexBlockContent() {
  const FakeIndexName comp{.name = compressed_index_entry_name,
                           .mft_ref = compressed_index_entry_mft_ref,
                           .directory = false};
  return MakeIndexBlockContent(std::span(&comp, 1));
}

// One $FILE_NAME of a BuildFakeNtfsImageWithMftTree() record.
struct FakeFileName {
  std::wstring_view name;
  ULONGLONG parent_ref;
  NtfsBrowser::Flag::FilenameNamespace name_space =
      NtfsBrowser::Flag::FilenameNamespace::Win32;
  // The size NTFS only refreshes in $FILE_NAME on a rename.
  ULONGLONG real_size = 0;
  // ORed onto the DIRECTORY/NONE flag WriteFileNameAttr() derives on its own,
  // eg. for a permission bit $FILE_NAME mirrors from $STANDARD_INFORMATION.
  NtfsBrowser::Flag::Filename extra_flags = NtfsBrowser::Flag::Filename::None;
};

// Writes one resident $FILE_NAME at record[offset] and returns its
// total_size.
DWORD WriteFileNameAttr(FakeRecord& record, DWORD offset,
                        const FakeFileName& name, bool directory) {
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::FileName;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size =
      gsl::narrow<DWORD>(offsetof(NtfsBrowser::Attr::Filename, name) +
                         name.name.size() * sizeof(WORD));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& filename = *reinterpret_cast<NtfsBrowser::Attr::Filename*>(
      &record.at(offset + attr.attr_offset));
  filename.parent_ref = name.parent_ref;
  filename.real_size = name.real_size;
  filename.alloc_size = name.real_size;
  filename.flags = (directory ? NtfsBrowser::Flag::Filename::Directory
                              : NtfsBrowser::Flag::Filename::None) |
                   name.extra_flags;
  filename.name_length = gsl::narrow<BYTE>(name.name.size());
  filename.name_space = name.name_space;
  for (size_t i = 0; i < name.name.size(); i++) {
    // i < name.name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name.name[i]);
  }

  return attr.header.total_size;
}

// Writes one resident, unnamed $DATA of size zero bytes at record[offset]
// and returns its total_size.
DWORD WriteResidentDataAttr(FakeRecord& record, DWORD offset, DWORD size) {
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);
  return attr.header.total_size;
}

// Builds one record of BuildFakeNtfsImageWithMftTree(): a header carrying
// sequence and baseRef, a $STANDARD_INFORMATION, names, and, when dataSize is
// set, a resident $DATA of that size.
FakeRecord MakeMftTreeRecord(NtfsBrowser::Flag::FileRecord flags, WORD sequence,
                             std::initializer_list<FakeFileName> names,
                             std::optional<DWORD> data_size = {},
                             NtfsBrowser::Flag::StdInfoPermission permission =
                                 NtfsBrowser::Flag::StdInfoPermission::Normal,
                             ULONGLONG base_ref = 0) {
  FakeRecord record = MakeRecordHeader(attr_offset_value, flags);
  EditFileRecordHeader(record, [&](FileRecordHeader::Data& header) {
    header.seq_no = sequence;
    header.ref_to_base = base_ref;
  });

  const bool directory = (flags & NtfsBrowser::Flag::FileRecord::Dir) ==
                         NtfsBrowser::Flag::FileRecord::Dir;

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(record, offset, permission);
  for (const FakeFileName& name : names) {
    offset += WriteFileNameAttr(record, offset, name, directory);
  }
  if (data_size) {
    offset += WriteResidentDataAttr(record, offset, *data_size);
  }

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImage() {
  NtfsBrowser::Data::NtfsBpb bpb{};
  std::memcpy(std::data(bpb.signature),
              NtfsBrowser::Data::ntfs_signature.data(), sizeof(bpb.signature));
  bpb.bytes_per_sector = bytes_per_sector;
  bpb.sectors_per_cluster = sectors_per_cluster;
  bpb.lcn_mft = mft_lcn;
  bpb.clusters_per_file_record = 1;
  bpb.clusters_per_index_block = 1;
  bpb.x_aa = boot_signature_low;
  bpb.x_55 = boot_signature_high;

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t records_end =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     (static_cast<size_t>(MftIdx::Root) + 1);
  // FullCache always reads a 64 KiB block regardless of length requested.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t image_size = std::max(records_end, full_cache_read_block_size);
  std::vector<BYTE> image(image_size, 0);
  std::memcpy(image.data(), &bpb, sizeof(bpb));

  const auto put_record = [&](MftIdx idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * static_cast<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };
  put_record(MftIdx::Mft, MakeMftRecord());
  put_record(MftIdx::Volume, MakeVolumeRecord());
  put_record(MftIdx::Root, MakeRootRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMinimalVolumeInformation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(MftIdx::Volume);
  const FakeRecord record =
      MakeVolumeRecordSized(minimal_volume_information_size);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEmptyVolumeInformation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(MftIdx::Volume);
  const FakeRecord record = MakeVolumeRecordSized(0);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithVolumeName() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(MftIdx::Volume);
  const FakeRecord record = MakeVolumeRecordWithName(fake_volume_name);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithDeletedVolumeRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(MftIdx::Volume);

  // Same $VOLUME_INFORMATION content as BuildFakeNtfsImage(), but with the
  // INUSE flag cleared - a freed record. bypass_deleted_gate_ must still let
  // NtfsVolume::Init() read it.
  FakeRecord record = MakeVolumeRecord();
  EditFileRecordHeader(record, [](FileRecordHeader::Data& header) {
    header.flags = NtfsBrowser::Flag::FileRecord{};
  });

  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithLegacyStandardInformation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset =
      mft_addr +
      static_cast<size_t>(fake_file_record_size) *
          static_cast<size_t>(legacy_standard_information_record_idx);
  const FakeRecord record =
      MakeStandardInformationRecordSized(legacy_standard_information_size);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEmptyStandardInformation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset =
      mft_addr +
      static_cast<size_t>(fake_file_record_size) *
          static_cast<size_t>(legacy_standard_information_record_idx);
  const FakeRecord record = MakeStandardInformationRecordSized(0);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithLegacyStandardInformationOnRoot() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(MftIdx::Root);
  const FakeRecord record =
      MakeStandardInformationRecordSized(legacy_standard_information_size);
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListDirectory() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attribute_list_dir_idx, MakeAttributeListOnlyDirRecord());
  put_record(index_extension_idx,
             MakeIndexRootExtensionRecord(attribute_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithUndersizedAttribute() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(undersized_attr_record_idx);
  const FakeRecord record = MakeUndersizedResidentAttrRecord();
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithForgedIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Only this fixture's directory needs an index block bigger than 1
  // cluster - patch the shared BPB in place (both fields are DWORD but
  // ntfs-volume.cpp::ParseBootSector() only ever consults their low byte,
  // truncated to a signed char) rather than duplicating
  // BuildFakeNtfsImage()'s whole boot sector setup.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block =
      static_cast<DWORD>(forged_index_block_size / cluster_size);

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t dir_offset =
      mft_addr +
      static_cast<size_t>(fake_file_record_size) * index_alloc_dir_idx;
  const FakeRecord dir_record = MakeIndexAllocDirRecord();
  std::memcpy(&image.at(dir_offset), dir_record.data(), dir_record.size());

  const size_t block_offset =
      static_cast<size_t>(forged_index_block_lcn) * cluster_size;
  if (image.size() < block_offset + forged_index_block_size) {
    image.resize(block_offset + forged_index_block_size, 0);
  }

  auto& block = *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(
      &image.at(block_offset));
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  block.offset_of_us = forged_index_block_offset_of_us;
  // size_of_us is never checked; this value only keeps the fixture plausible.
  block.size_of_us =
      static_cast<WORD>(forged_index_block_size / bytes_per_sector + 1);
  block.vcn = 0;
  block.not_leaf = 0;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithTinyIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = tiny_clusters_per_index_block;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = oversized_clusters_per_index_block;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedFileRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_file_record = oversized_clusters_per_file_record;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFileRecordSizeTooBig() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // ParseBootSector() reads this DWORD field's low byte as a signed char.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_file_record =
      file_record_size_too_big_clusters_per_file_record;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeMftLcn() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // lcn_mft is otherwise only ever set once, in BuildFakeNtfsImage() itself.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.lcn_mft = huge_mft_lcn;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiTypeAttributeListDirectory() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attr_list_multi_type_dir_idx,
             MakeAttributeListTwoTypesDirRecord());
  put_record(multi_type_extension_idx, MakeIndexRootAndAllocExtensionRecord(
                                           attr_list_multi_type_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedAttributeListDirectory() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(uaf_attr_list_dir_idx, MakeFragmentedAttributeListDirRecord());
  put_record(uaf_extension_idx0,
             MakeIndexAllocationOnlyExtensionRecord(
                 uaf_real_size_sentinels.at(0), uaf_attr_list_dir_idx));
  put_record(uaf_extension_idx1,
             MakeIndexAllocationOnlyExtensionRecord(
                 uaf_real_size_sentinels.at(1), uaf_attr_list_dir_idx));
  put_record(uaf_extension_idx2,
             MakeIndexAllocationOnlyExtensionRecord(
                 uaf_real_size_sentinels.at(2), uaf_attr_list_dir_idx));
  put_record(uaf_extension_idx3,
             MakeIndexAllocationOnlyExtensionRecord(
                 uaf_real_size_sentinels.at(3), uaf_attr_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptMftRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Zeroing the magic makes ParseFileRecord() fail on this record alone.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset = mft_addr + static_cast<size_t>(fake_file_record_size) *
                                       static_cast<size_t>(MftIdx::Mft);
  std::memset(&image.at(offset), 0, fake_file_record_size);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttrNameExceedsTotalSize() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t offset =
      mft_addr +
      static_cast<size_t>(fake_file_record_size) *
          static_cast<size_t>(attr_name_exceeds_total_size_record_idx);
  const FakeRecord record = MakeAttrNameExceedsTotalSizeRecord();
  std::memcpy(&image.at(offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttrOffsetOutOfBounds() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // offset_of_attr is per-record, unlike the BPB fields patched above.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  EditFileRecordHeader(
      std::span(image).subspan(root_offset, fake_file_record_size),
      [](FileRecordHeader::Data& header) {
        header.offset_of_attr = attr_offset_out_of_bounds;
      });

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSmallResidentData() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeSmallResidentDataRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListShortRead() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeAttributeListShortReadRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListRecordSizeTooSmall() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attribute_list_dir_idx,
             MakeAttributeListRecordSizeTooSmallDirRecord());
  put_record(index_extension_idx,
             MakeIndexRootExtensionRecord(attribute_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListOffsetMismatch() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attribute_list_dir_idx,
             MakeAttributeListOffsetMismatchDirRecord());
  put_record(index_extension_idx,
             MakeIndexRootExtensionRecord(attribute_list_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttributeListCycle() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(static_cast<ULONGLONG>(MftIdx::Root),
             MakeAttributeListCycleRecord(attr_list_cycle_ext_idx));
  FakeRecord ext =
      MakeAttributeListCycleRecord(static_cast<ULONGLONG>(MftIdx::Root));
  SetRecordLink(ext, 0, static_cast<ULONGLONG>(MftIdx::Root));
  put_record(attr_list_cycle_ext_idx, ext);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithTightlyPackedAttributeListDirectory() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(attr_list_tight_pack_dir_idx,
             MakeAttributeListTightlyPackedDirRecord());
  put_record(attr_list_tight_pack_ext_idx_a,
             MakeIndexRootExtensionRecord(attr_list_tight_pack_dir_idx));
  put_record(attr_list_tight_pack_ext_idx_b,
             MakeIndexAllocationOnlyExtensionRecord(
                 attr_list_tight_pack_real_size, attr_list_tight_pack_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptRootRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Zeroes the root directory's (#5) own record, so ParseFileRecord(ROOT)
  // fails while $Volume and $MFT stay valid.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  std::memset(&image.at(root_offset), 0, fake_file_record_size);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedMftInvalidRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // VCN 0..fragmented_mft_invalid_record_idx, inclusive.
  constexpr DWORD clusters_value =
      static_cast<DWORD>(fragmented_mft_invalid_record_idx) + 1;

  // Overwrites $MFT's own record with one whose DATA attribute has a real
  // data run.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const FakeRecord mft_record =
      MakeMftRecordWithRealDataRun(fragmented_mft_data_run_lcn, clusters_value);
  std::memcpy(&image.at(mft_addr), mft_record.data(), mft_record.size());

  // Written at the physical cluster the data run maps this record's VCN to.
  const size_t forged_offset =
      (static_cast<size_t>(fragmented_mft_data_run_lcn) +
       fragmented_mft_invalid_record_idx) *
      cluster_size;
  if (image.size() < forged_offset + fake_file_record_size) {
    image.resize(forged_offset + fake_file_record_size, 0);
  }
  const FakeRecord forged_record = MakeInvalidOffsetOfUsRecord();
  std::memcpy(&image.at(forged_offset), forged_record.data(),
              forged_record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataSplitAcrossAttributeList() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  // Overwrites $MFT's own record: a $ATTRIBUTE_LIST relocating its DATA
  // continuation (starting at mft_data_split_target_idx) to the extension
  // record below.
  const std::array<std::pair<ULONGLONG, ULONGLONG>, 1> continuations{
      {{mft_data_split_ext_idx, mft_data_split_target_idx}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(MftIdx::Mft),
               MakeMftRecordWithDataContinuations(continuations));

  // Extension record: the continuation instance itself, one cluster
  // starting at VCN mft_data_split_target_idx, mapped to mft_data_split_lcn.
  PutMftRecord(image, mft_addr, mft_data_split_ext_idx,
               MakeMftDataContinuationExtensionRecord(mft_data_split_target_idx,
                                                      mft_data_split_lcn, 1));

  // The target file record itself, at the physical LCN the continuation
  // instance's data run maps its VCN to - only reachable by consulting that
  // instance, since mft_data_split_target_idx is past Enum::MftIdx::USER.
  FakeRecord target_record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  WriteEndOfAttributesMarker(target_record, attr_offset_value);
  PutRecordAt(image, static_cast<size_t>(mft_data_split_lcn) * cluster_size,
              target_record);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataExtentChain() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  // Entry 1 names mft_chain_ext_a, reachable only through entry 2's extent.
  const std::array<std::pair<ULONGLONG, ULONGLONG>, 2> continuations{
      {{mft_chain_ext_a, mft_chain_ext_a_start_vcn},
       {mft_chain_ext_b, mft_chain_ext_b_start_vcn}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(MftIdx::Mft),
               MakeMftRecordWithDataContinuations(continuations));

  // Ext B: naively reachable (below Enum::MftIdx::USER). Its own extent
  // covers mft_chain_ext_a's real location.
  PutMftRecord(image, mft_addr, mft_chain_ext_b,
               MakeMftDataContinuationExtensionRecord(
                   mft_chain_ext_b_start_vcn, mft_chain_ext_b_lcn,
                   mft_chain_ext_b_clusters));

  // Ext A's real bytes, mapped through ext B's extent; naive slot stays zero.
  const size_t ext_a_offset = (static_cast<size_t>(mft_chain_ext_b_lcn) +
                               (mft_chain_ext_a - mft_chain_ext_b_start_vcn)) *
                              cluster_size;
  PutRecordAt(image, ext_a_offset,
              MakeMftDataContinuationExtensionRecord(mft_chain_ext_a_start_vcn,
                                                     mft_chain_target_lcn, 1));

  // The target file record, only reachable once both hops resolve.
  FakeRecord target_record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  WriteEndOfAttributesMarker(target_record, attr_offset_value);
  PutRecordAt(image, static_cast<size_t>(mft_chain_target_lcn) * cluster_size,
              target_record);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithUnresolvableMftDataExtent() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  // Entry 1 is permanently unresolvable; entry 2 resolves fine.
  const std::array<std::pair<ULONGLONG, ULONGLONG>, 2> continuations{
      {{mft_unresolvable_ext_idx, mft_unresolvable_start_vcn},
       {mft_unresolvable_good_ext_idx, mft_unresolvable_good_start_vcn}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(MftIdx::Mft),
               MakeMftRecordWithDataContinuations(continuations));

  // The good entry: naively reachable, its extent covers the good record.
  PutMftRecord(image, mft_addr, mft_unresolvable_good_ext_idx,
               MakeMftDataContinuationExtensionRecord(
                   mft_unresolvable_good_start_vcn, mft_unresolvable_good_lcn,
                   mft_unresolvable_good_clusters));
  FakeRecord good_record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  WriteEndOfAttributesMarker(good_record, attr_offset_value);
  PutRecordAt(
      image,
      (static_cast<size_t>(mft_unresolvable_good_lcn) +
       (mft_unresolvable_good_record - mft_unresolvable_good_start_vcn)) *
          cluster_size,
      good_record);

  // mft_unresolvable_ext_idx's slot stays zero-filled and unmapped.

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataLastVcnOverflow() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  const std::array<std::pair<ULONGLONG, ULONGLONG>, 1> continuations{
      {{mft_last_vcn_overflow_target_idx, 0}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(MftIdx::Mft),
               MakeMftRecordWithDataContinuations(
                   continuations, mft_last_vcn_overflow_last_vcn));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithNamedDataStream() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeNamedDataStreamRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithIndexRootVariants() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  put_record(index_root_variant_a_dir_idx,
             MakeIndexRootDirRecord(index_root_variant_a_name,
                                    index_root_variant_a_mft_ref,
                                    index_root_variant_a_dir_idx));
  put_record(index_root_variant_b_dir_idx,
             MakeIndexRootDirRecord(index_root_variant_b_name,
                                    index_root_variant_b_mft_ref,
                                    index_root_variant_b_dir_idx));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithRootIndexRootEntry() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeIndexRootExtensionRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithGapCollationSubNode() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeRootRecordWithGapCollationSubNode();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  // The sub-node itself: one real index block holding
  // gap_collation_search_name as its only leaf entry.
  const size_t block_offset =
      static_cast<size_t>(gap_collation_index_block_lcn) * cluster_size;
  if (image.size() < block_offset + cluster_size) {
    image.resize(block_offset + cluster_size, 0);
  }

  const std::span<BYTE> block_start =
      std::span<BYTE>(image).subspan(block_offset);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points at the block's own last 6 bytes, so PatchUS() succeeds
  // trivially without a real fixup array.
  block.offset_of_us = static_cast<WORD>(cluster_size - us_slot_size);
  block.size_of_us = 3;
  block.vcn = 0;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;

  const std::span<BYTE> body =
      block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));

  // Entry 1: the real leaf entry, a plain leaf with no sub-node.
  auto& first_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());
  first_entry.mft_index = gap_collation_leaf_mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = static_cast<ULONGLONG>(MftIdx::Root);
  fn1.flags = NtfsBrowser::Flag::Filename::None;
  fn1.name_length = gap_collation_search_name_length;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  for (BYTE i = 0; i < fn1.name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(gap_collation_search_name[i]);
  }

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  // Entry 2: the terminating entry - no name, no sub-node.
  auto& second_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, first_entry.size));
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
  second_entry.stream_size = 0;
  second_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &second_entry.stream - reinterpret_cast<BYTE*>(&second_entry)));

  block.total_entry_size =
      static_cast<DWORD>(first_entry.size) + second_entry.size;
  block.alloc_entry_size = block.total_entry_size;

  return image;
}

namespace {

// UTF-16 units in $UpCase: one entry per code unit of the BMP.
constexpr size_t up_case_unit_count = 65536;

// LCN of the $INDEX_ALLOCATION block the non-ASCII fixture files its names in.
constexpr DWORD non_ascii_index_block_lcn = 20;

// LCN of $UpCase's data in the non-ASCII fixture, past every record above.
constexpr DWORD non_ascii_up_case_lcn = 64;

// The 128 KiB $UpCase image the non-ASCII fixture stores: the identity map,
// but for a-z, the Latin-1 letters and y-diaeresis. The dotless i stays
// unmapped, as it does in a Windows table. Independent of the library.
std::vector<BYTE> MakeNonAsciiUpCaseBytes() {
  constexpr WORD latin1_lower_first = 0x00E0;
  constexpr WORD latin1_lower_last = 0x00FE;
  constexpr WORD division_sign = 0x00F7;
  constexpr WORD y_diaeresis = 0x00FF;
  constexpr WORD capital_y_diaeresis = 0x0178;
  constexpr WORD case_distance = 0x20;

  std::vector<BYTE> bytes(up_case_unit_count * sizeof(WORD));
  for (size_t unit = 0; unit < up_case_unit_count; unit++) {
    auto upper = gsl::narrow<WORD>(unit);
    if ((unit >= L'a' && unit <= L'z') ||
        (unit >= latin1_lower_first && unit <= latin1_lower_last &&
         unit != division_sign)) {
      upper = gsl::narrow<WORD>(unit - case_distance);
    } else if (unit == y_diaeresis) {
      upper = capital_y_diaeresis;
    }
    bytes.at(unit * 2) = static_cast<BYTE>(upper & byte_mask);
    bytes.at(unit * 2 + 1) = static_cast<BYTE>(upper >> bits_per_byte);
  }
  return bytes;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithNonAsciiNames(NonAsciiNameLayout layout,
                                                      bool with_up_case) {
  // Sorted by uppercase: E-acute (C9), O-diaeresis (D6), dotless i (131).
  const std::array<FakeIndexName, 3> names{
      {{non_ascii_acute_name, non_ascii_acute_mft_ref, false},
       {non_ascii_diaeresis_name, non_ascii_diaeresis_mft_ref, false},
       {non_ascii_dotless_name, non_ascii_dotless_mft_ref, false}}};

  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](MftIdx idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * static_cast<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  if (layout == NonAsciiNameLayout::IndexRoot) {
    put_record(MftIdx::Root, MakeIndexRootDirRecordWithNames(names));
  } else {
    const std::vector<FakeDataRun> runs{{non_ascii_index_block_lcn, 1}};
    put_record(MftIdx::Root, MakeIndexAllocationDirRecord(
                                 NtfsBrowser::Flag::StdInfoPermission::Archive,
                                 0, fake_file_record_size, runs));
    LayRunBytes(image, runs, MakeIndexBlockContent(names));
  }

  if (with_up_case) {
    // $MFT now maps up to record 10, so $UpCase can be read through it.
    put_record(MftIdx::Mft, MakeMftRecordWithRealDataRun(
                                static_cast<DWORD>(mft_lcn),
                                static_cast<DWORD>(MftIdx::UpCase) + 1));

    const std::vector<BYTE> table = MakeNonAsciiUpCaseBytes();
    const std::vector<FakeDataRun> runs{
        {non_ascii_up_case_lcn,
         gsl::narrow<DWORD>(table.size() / fake_cluster_size)}};

    FakeRecord upcase = MakeRecordHeader(attr_offset_value,
                                         NtfsBrowser::Flag::FileRecord::InUse);
    const DWORD attr_size = WriteNonResidentAttr(
        upcase, attr_offset_value, AttrType::Data, 0, table.size(), runs);
    WriteEndOfAttributesMarker(upcase, attr_offset_value + attr_size);
    put_record(MftIdx::UpCase, upcase);

    LayRunBytes(image, runs, table);
  }

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithDeepIndexBlockChain() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Replace the root directory's (#5) whole record in place.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeIndexBlockChainRootRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  // The chain itself: index_block_chain_length contiguous blocks starting at
  // index_block_chain_lcn, one cluster each.
  const size_t chain_offset =
      static_cast<size_t>(index_block_chain_lcn) * cluster_size;
  const size_t chain_bytes =
      static_cast<size_t>(index_block_chain_length) * cluster_size;
  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the chain's real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t chain_end = chain_offset + chain_bytes;
  const size_t aligned_chain_end =
      ((chain_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_chain_end) {
    image.resize(aligned_chain_end, 0);
  }

  for (DWORD vcn = 0; vcn < index_block_chain_length; vcn++) {
    const std::span<BYTE> block_start = std::span<BYTE>(image).subspan(
        chain_offset + static_cast<size_t>(vcn) * cluster_size);
    auto& block =
        *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
    std::memset(&block, 0, sizeof(block));
    block.magic = index_block_magic;
    // Points offset_of_us at the fixup slot itself, valid for every block.
    block.offset_of_us = static_cast<WORD>(cluster_size - us_slot_size);
    block.size_of_us = 3;
    block.vcn = vcn;
    block.entry_offset = gsl::narrow<DWORD>(
        (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
        reinterpret_cast<BYTE*>(&block.entry_offset));

    const std::span<BYTE> body =
        block_start.subspan(sizeof(NtfsBrowser::Data::IndexBlock));
    auto& first_entry =
        *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(body.data());

    const bool is_leaf = (vcn == index_block_chain_length - 1);
    if (!is_leaf) {
      // Intermediate block: a lone, nameless entry pointing at the next VCN.
      block.not_leaf = 1;
      first_entry.mft_index = 0;
      first_entry.mft_sn = 0;
      first_entry.stream_size = 0;
      first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode |
                          NtfsBrowser::Flag::IndexEntry::Last;
      first_entry.size = static_cast<WORD>(AlignAttrSize(
          offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
      auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
      sub_node_vcn = vcn + 1;
    } else {
      // Deepest block: the real, named leaf entry, reached by depth alone.
      block.not_leaf = 0;
      first_entry.mft_index = index_block_chain_leaf_mft_ref;
      first_entry.mft_sn = 1;

      auto& fn1 =
          *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
      fn1.parent_ref = static_cast<ULONGLONG>(MftIdx::Root);
      fn1.flags = NtfsBrowser::Flag::Filename::None;
      fn1.name_length = index_block_chain_leaf_name_length;
      fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
      for (BYTE i = 0; i < fn1.name_length; i++) {
        // i is below name_length, the length of the name.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        fn1.name[i] = gsl::narrow<WORD>(index_block_chain_leaf_name[i]);
      }

      first_entry.stream_size = gsl::narrow<WORD>(
          reinterpret_cast<BYTE*>(&fn1.name[fn1.name_length]) -
          reinterpret_cast<BYTE*>(&fn1));
      first_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
      first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
          &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
          first_entry.stream_size));
    }

    block.total_entry_size = first_entry.size;
    block.alloc_entry_size = first_entry.size;
  }

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedIndexBlocks() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Replace the root directory's (#5) whole record in place.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeOrphanedIndexBlocksRootRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  const size_t blocks_offset =
      static_cast<size_t>(orphaned_blocks_lcn) * cluster_size;
  const size_t blocks_bytes =
      static_cast<size_t>(orphaned_blocks_count) * cluster_size;
  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t blocks_end = blocks_offset + blocks_bytes;
  const size_t aligned_blocks_end =
      ((blocks_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_blocks_end) {
    image.resize(aligned_blocks_end, 0);
  }

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::Root), root_sequence_number);

  // VCN 0: reachable through $INDEX_ROOT's own sub-node pointer.
  WriteOrphanedIndexLeafBlock(image, 0, orphaned_block_reachable_mft_ref,
                              root_ref, orphaned_block_reachable_name);
  // VCN 1: orphaned, but still filed under this directory.
  WriteOrphanedIndexLeafBlock(image, 1, orphaned_block_orphan_mft_ref, root_ref,
                              orphaned_block_orphan_name);
  // VCN 2: orphaned, and filed under a different parent - a recovery scan
  // must find the block but reject the entry.
  WriteOrphanedIndexLeafBlock(
      image, 2, orphaned_block_stale_mft_ref,
      MakeFileReference(orphaned_block_stale_parent_ref, root_sequence_number),
      orphaned_block_stale_name);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeOrphanScanBlockCount() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Replace the root directory's (#5) $INDEX_ALLOCATION real_size only: the
  // three real blocks (and $INDEX_ROOT's own pointer to VCN 0) stay exactly
  // as BuildFakeNtfsImageWithOrphanedIndexBlocks() wrote them.
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record =
      MakeOrphanedIndexBlocksRootRecord(huge_orphan_scan_declared_block_count);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithOrphanedIndexBlockSequenceMismatch() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  // Give orphaned_block_sequence_mismatch_target_idx a real, in-use record on
  // disk, under orphaned_block_sequence_mismatch_record_seq.
  const FakeRecord record =
      MakeMftTreeRecord(NtfsBrowser::Flag::FileRecord::InUse,
                        orphaned_block_sequence_mismatch_record_seq, {});
  PutMftRecord(image, static_cast<DWORD>(mft_lcn) * cluster_size,
               orphaned_block_sequence_mismatch_target_idx, record);

  // Redirect the "Orphan" entry (VCN 1) to name that record instead of
  // orphaned_block_orphan_mft_ref, leaving its mft_sn exactly as
  // WriteOrphanedIndexLeafBlock() wrote it (1) - now a mismatch against the
  // record's own sequence number (orphaned_block_sequence_mismatch_record_seq).
  const size_t vcn1_offset =
      (static_cast<size_t>(orphaned_blocks_lcn) + 1) * cluster_size;
  auto& entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &image.at(vcn1_offset + sizeof(NtfsBrowser::Data::IndexBlock)));
  entry.mft_index = orphaned_block_sequence_mismatch_target_idx;

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiClusterOrphanedIndexBlock() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Only this fixture's directory needs an index block bigger than 1
  // cluster - patch the shared BPB in place, the same trick
  // BuildFakeNtfsImageWithForgedIndexBlock() uses.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block =
      static_cast<DWORD>(multi_cluster_orphan_clusters_per_block);

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeMultiClusterOrphanedIndexBlocksRootRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  const size_t blocks_offset =
      static_cast<size_t>(multi_cluster_orphan_lcn) * cluster_size;
  const size_t blocks_bytes =
      static_cast<size_t>(multi_cluster_orphan_block_count) *
      multi_cluster_orphan_index_block_size;
  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t blocks_end = blocks_offset + blocks_bytes;
  const size_t aligned_blocks_end =
      ((blocks_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_blocks_end) {
    image.resize(aligned_blocks_end, 0);
  }

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::Root), root_sequence_number);

  WriteMultiClusterIndexLeafBlock(image, 0, multi_cluster_reachable_mft_ref,
                                  root_ref, multi_cluster_reachable_name);
  WriteMultiClusterIndexLeafBlock(image, 1, multi_cluster_orphan_mft_ref,
                                  root_ref, multi_cluster_orphan_name);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSubClusterOrphanedIndexBlocks() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Patch the shared BPB: index blocks of 2^9 bytes, half a cluster.
  auto& bpb = *reinterpret_cast<NtfsBrowser::Data::NtfsBpb*>(image.data());
  bpb.clusters_per_index_block = sub_cluster_index_block_encoding;

  constexpr DWORD index_block_size = 512;
  constexpr DWORD alloc_lcn = 220;
  const std::array<FakeIndexAllocExtent, 1> extents{
      FakeIndexAllocExtent{.start_vcn = 0,
                           .clusters = sub_cluster_alloc_clusters,
                           .lcn = alloc_lcn}};
  const FakeRecord record = MakeDirectoryWithIndexAllocation(
      index_block_size, extents, sub_cluster_block_names.size());
  const size_t root_offset = static_cast<size_t>(mft_lcn) * cluster_size +
                             static_cast<size_t>(fake_file_record_size) *
                                 static_cast<size_t>(MftIdx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  const size_t blocks_offset = static_cast<size_t>(alloc_lcn) * cluster_size;
  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t blocks_end =
      blocks_offset + sub_cluster_block_names.size() * index_block_size;
  const size_t aligned_blocks_end =
      ((blocks_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_blocks_end) {
    image.resize(aligned_blocks_end, 0);
  }

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::Root), root_sequence_number);
  for (size_t i = 0; i < sub_cluster_block_names.size(); i++) {
    WriteIndexLeafBlockAt(image, blocks_offset + i * index_block_size,
                          index_block_size, i,
                          sub_cluster_block_record_base + i, root_ref,
                          sub_cluster_block_names.at(i));
  }

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSplitIndexAllocation() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Index blocks are one cluster each: block i is VCN i.
  constexpr DWORD first_lcn = 230;
  constexpr DWORD second_lcn = 240;
  const std::array<FakeIndexAllocExtent, 2> extents{
      FakeIndexAllocExtent{
          .start_vcn = 0, .clusters = split_extent_clusters, .lcn = first_lcn},
      FakeIndexAllocExtent{.start_vcn = split_extent_clusters,
                           .clusters = split_extent_clusters,
                           .lcn = second_lcn}};
  const FakeRecord record = MakeDirectoryWithIndexAllocation(
      cluster_size, extents, split_block_names.size());
  const size_t root_offset = static_cast<size_t>(mft_lcn) * cluster_size +
                             static_cast<size_t>(fake_file_record_size) *
                                 static_cast<size_t>(MftIdx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t blocks_end =
      static_cast<size_t>(second_lcn + split_extent_clusters) * cluster_size;
  const size_t aligned_blocks_end =
      ((blocks_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_blocks_end) {
    image.resize(aligned_blocks_end, 0);
  }

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::Root), root_sequence_number);
  for (size_t i = 0; i < split_block_names.size(); i++) {
    const size_t lcn = (i < split_extent_clusters)
                           ? first_lcn + i
                           : second_lcn + (i - split_extent_clusters);
    WriteIndexLeafBlockAt(image, lcn * cluster_size, cluster_size, i,
                          split_block_record_base + i, root_ref,
                          split_block_names.at(i));
  }

  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithOrphanedIndexBlockParentLink(FakeParentLink link) {
  std::vector<BYTE> image = BuildFakeNtfsImageWithOrphanedIndexBlocks();

  const size_t root_offset = static_cast<size_t>(mft_lcn) * cluster_size +
                             static_cast<size_t>(fake_file_record_size) *
                                 static_cast<size_t>(MftIdx::Root);
  EditFileRecordHeader(
      std::span(image).subspan(root_offset, fake_file_record_size),
      [&](FileRecordHeader::Data& header) {
        header.seq_no = link.record_sequence;
        header.flags = link.record_in_use
                           ? (NtfsBrowser::Flag::FileRecord::InUse |
                              NtfsBrowser::Flag::FileRecord::Dir)
                           : NtfsBrowser::Flag::FileRecord::Dir;
      });

  // VCN 1's entry names the directory unchecked (sequence 0), so it is
  // reported whatever generation the record is on.
  WriteOrphanedIndexLeafBlock(
      image, 1, orphaned_block_orphan_mft_ref,
      MakeFileReference(static_cast<ULONGLONG>(MftIdx::Root), 0),
      orphaned_block_orphan_name);

  // Replace VCN 2's foreign-parent entry: same directory, this generation.
  WriteOrphanedIndexLeafBlock(
      image, 2, orphaned_block_generation_mft_ref,
      MakeFileReference(static_cast<ULONGLONG>(MftIdx::Root),
                        link.entry_parent_sequence),
      orphaned_block_generation_name);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftTree() {
  using NtfsBrowser::Flag::FilenameNamespace;
  using NtfsBrowser::Flag::StdInfoPermission;
  using RecordFlag = NtfsBrowser::Flag::FileRecord;

  // Sequence numbers the records below carry, where another record's
  // parent reference names them.
  constexpr WORD docs_sequence = 1;
  constexpr WORD old_dir_sequence = 3;
  constexpr WORD report_sequence = 3;
  constexpr WORD new_dir_sequence = 7;

  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const auto put_record = [&](ULONGLONG idx, const FakeRecord& record) {
    const size_t offset =
        mft_addr +
        static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
    std::memcpy(&image.at(offset), record.data(), record.size());
  };

  const ULONGLONG root = MakeFileReference(static_cast<ULONGLONG>(MftIdx::Root),
                                           root_sequence_number);
  const ULONGLONG docs = MakeFileReference(mft_tree_docs_idx, docs_sequence);

  // $MFT: a data run over every slot, written in place, so the records past
  // the first 16 read through it; plus the name real volumes give it.
  FakeRecord mft = MakeMftRecordWithRealDataRun(
      static_cast<DWORD>(mft_lcn), static_cast<DWORD>(mft_tree_record_count));
  EditFileRecordHeader(
      mft, [](FileRecordHeader::Data& header) { header.seq_no = 1; });
  DWORD offset = attr_offset_value +
                 reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
                     &mft.at(attr_offset_value))
                     ->header.total_size;
  offset += WriteFileNameAttr(mft, offset,
                              {.name = L"$MFT", .parent_ref = root}, false);
  WriteEndOfAttributesMarker(mft, offset);
  put_record(static_cast<ULONGLONG>(MftIdx::Mft), mft);

  const RecordFlag file = RecordFlag::InUse;
  const RecordFlag dir = RecordFlag::InUse | RecordFlag::Dir;
  const RecordFlag deleted_file{};
  const RecordFlag deleted_dir = RecordFlag::Dir;

  put_record(static_cast<ULONGLONG>(MftIdx::Root),
             MakeMftTreeRecord(dir, root_sequence_number,
                               {{.name = L".", .parent_ref = root}}));
  put_record(mft_tree_docs_idx,
             MakeMftTreeRecord(dir, docs_sequence,
                               {{.name = L"Docs", .parent_ref = root}}));
  put_record(mft_tree_report_idx,
             MakeMftTreeRecord(
                 file, report_sequence,
                 {{.name = L"report.txt",
                   .parent_ref = docs,
                   .real_size = mft_tree_report_stale_size,
                   .extra_flags = NtfsBrowser::Flag::Filename::ReadOnly |
                                  NtfsBrowser::Flag::Filename::Archive},
                  {.name = L"REPORT~1.TXT",
                   .parent_ref = docs,
                   .name_space = FilenameNamespace::Dos}},
                 mft_tree_report_data_size,
                 StdInfoPermission::ReadOnly | StdInfoPermission::Archive));
  put_record(mft_tree_hard_link_idx,
             MakeMftTreeRecord(file, 1,
                               {{.name = L"link-a", .parent_ref = root},
                                {.name = L"link-b", .parent_ref = docs}}));
  // NTFS bumps a record's sequence number when it frees the record.
  put_record(mft_tree_deleted_file_idx,
             MakeMftTreeRecord(deleted_file, 2,
                               {{.name = L"old.tmp", .parent_ref = docs}}));
  put_record(mft_tree_deleted_dir_idx,
             MakeMftTreeRecord(deleted_dir, old_dir_sequence + 1,
                               {{.name = L"OldDir", .parent_ref = docs}}));
  put_record(
      mft_tree_deleted_child_idx,
      MakeMftTreeRecord(deleted_file, 2,
                        {{.name = L"draft.doc",
                          .parent_ref = MakeFileReference(
                              mft_tree_deleted_dir_idx, old_dir_sequence)}}));
  put_record(mft_tree_stale_child_idx,
             MakeMftTreeRecord(
                 deleted_file, 2,
                 {{.name = L"stale.txt",
                   .parent_ref = MakeFileReference(mft_tree_reused_dir_idx,
                                                   new_dir_sequence - 1)}}));
  put_record(mft_tree_reused_dir_idx,
             MakeMftTreeRecord(dir, new_dir_sequence,
                               {{.name = L"NewDir", .parent_ref = root}}));
  put_record(mft_tree_extension_idx,
             MakeMftTreeRecord(
                 file, 1, {{.name = L"ext", .parent_ref = docs}}, {},
                 StdInfoPermission::Normal,
                 MakeFileReference(mft_tree_report_idx, report_sequence)));

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftExtensionRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();

  // $MFT's sequence number, which BuildFakeNtfsImageWithMftTree() sets to 1.
  constexpr WORD mft_sequence = 1;
  const FakeRecord record = MakeMftTreeRecord(
      NtfsBrowser::Flag::FileRecord::InUse, mft_sequence, {}, {},
      NtfsBrowser::Flag::StdInfoPermission::Normal,
      MakeFileReference(static_cast<ULONGLONG>(MftIdx::Mft), mft_sequence));

  const size_t offset = static_cast<size_t>(mft_lcn) * cluster_size +
                        static_cast<size_t>(fake_file_record_size) *
                            static_cast<size_t>(mft_tree_zeroed_idx);
  std::memcpy(&image.at(offset), record.data(), record.size());
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithHugeMftRealSize() {
  std::vector<BYTE> image = BuildFakeNtfsImageWithMftTree();

  const size_t mft_offset = static_cast<size_t>(mft_lcn) * cluster_size;
  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &image.at(mft_offset + attr_offset_value));
  attr.real_size = huge_mft_real_size;
  return image;
}

std::vector<BYTE> MakeUncompressedLznt1Chunk(std::span<const BYTE> payload) {
  // [MS-XCA] section 2.5.3: input streams are compressed in units of 4096
  // bytes, so a single chunk never carries more than that (and a chunk with
  // no payload at all is not representable - the declared size is
  // payload.size() - 1).
  assert(!payload.empty() && payload.size() <= NtfsBrowser::Lznt1::chunk_size);

  // Header: bit 15 clear (uncompressed), bits 14-12 == 3 (signature), bits
  // 11-0 == payload size - 1 (the whole chunk's size, header included, minus
  // three) - [MS-XCA] section 2.5.1.2.
  const auto header =
      gsl::narrow<WORD>(0x3000U | gsl::narrow<unsigned>(payload.size() - 1));

  std::vector<BYTE> chunk;
  chunk.reserve(payload.size() + 2);
  chunk.push_back(static_cast<BYTE>(header & byte_mask));
  chunk.push_back(static_cast<BYTE>(header >> bits_per_byte));
  chunk.insert(chunk.end(), payload.begin(), payload.end());
  return chunk;
}

std::vector<BYTE> CompressionFixturePattern(size_t size) {
  std::vector<BYTE> pattern(size, 0);
  for (size_t i = 0; i < size; i++) {
    // Deliberately not a byte-aligned cycle, so a fixture whose content got
    // shifted by a whole number of bytes/clusters still compares unequal.
    pattern.at(i) =
        static_cast<BYTE>((i * pattern_mul + pattern_add) % pattern_mod);
  }
  return pattern;
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedFile() {
  // One real cluster (the [MS-XCA] section 3.3 worked example's 59
  // compressed bytes) plus a sparse pad up to a whole compression unit -
  // 1 < compression_unit_clusters real clusters is exactly what marks a unit
  // as compressed rather than stored.
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, xca_lznt1_example_decompressed_size, runs);

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithStoredCompressionUnit() {
  // Real runs covering the whole unit, no sparse pad: a stored
  // (incompressible) unit, whose bytes must come back untouched.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, compression_unit_clusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, compression_unit_size, runs);

  return BuildCompressionImage(
      record, runs, CompressionFixturePattern(compression_unit_size));
}

std::vector<BYTE> BuildFakeNtfsImageWithUninitializedTail() {
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, uninitialized_tail_clusters}};

  const FakeRecord record =
      MakeNonResidentDataRecord(NtfsBrowser::Flag::StdInfoPermission::Archive,
                                0, uninitialized_tail_real_size, runs,
                                {.ini_size = uninitialized_tail_ini_size});

  return BuildCompressionImage(
      record, runs,
      CompressionFixturePattern(size_t{uninitialized_tail_clusters} *
                                cluster_size));
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiClusterBitmap() {
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, multi_cluster_bitmap_clusters}};

  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NtfsBrowser::Flag::StdInfoPermission::Archive);
  offset += WriteNonResidentAttr(
      record, offset, AttrType::Bitmap, 0,
      static_cast<ULONGLONG>(multi_cluster_bitmap_clusters) * cluster_size,
      runs);
  WriteEndOfAttributesMarker(record, offset);

  std::vector<BYTE> bitmap(
      static_cast<size_t>(multi_cluster_bitmap_clusters) * cluster_size, 0x00);
  std::fill_n(bitmap.begin(), cluster_size, all_bits_set);
  bitmap.at(static_cast<size_t>(2) * cluster_size) = 0x01;

  return BuildCompressionImage(record, runs, bitmap);
}

std::vector<BYTE> BuildFakeNtfsImageWithSparseCompressionUnit() {
  const std::vector<FakeDataRun> runs{{{}, compression_unit_clusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed |
          NtfsBrowser::Flag::StdInfoPermission::Sparse,
      compression_unit_size_shift, compression_unit_size, runs);

  return BuildCompressionImage(record, runs, {});
}

std::vector<BYTE> BuildFakeNtfsImageWithFragmentedCompressedFile() {
  // Two non-contiguous single-cluster real runs (so the compressed bytes
  // genuinely span two Data::RunEntrys) plus a 2-cluster sparse pad.
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {fragmented_compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fragmented_compressed_payload_size, runs);

  const std::vector<BYTE> payload =
      CompressionFixturePattern(fragmented_compressed_payload_size);
  return BuildCompressionImage(record, runs,
                               MakeUncompressedLznt1Chunk(payload));
}

std::vector<BYTE> BuildFakeNtfsImageWithTrailingPartialCompressionUnit() {
  // Five real clusters then one sparse: unit 0 (VCN 0..3) is entirely real
  // (stored), and unit 1 (VCN 4..5 only - the attribute stops there) sees
  // the tail of that same run plus one hole, so it is a compressed unit
  // whose real extent comes from a PARTIAL run rather than a run of its own.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, compression_unit_clusters + 1}, {{}, 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift,
      trailing_partial_unit_stored_size + trailing_partial_unit_tail_size,
      runs);

  // Clusters 0..3 hold unit 0's stored bytes verbatim; cluster 4 holds unit
  // 1's LZNT1 stream (a single hand-encoded uncompressed chunk).
  std::vector<BYTE> cluster_bytes =
      CompressionFixturePattern(trailing_partial_unit_stored_size);
  const std::vector<BYTE> tail = MakeUncompressedLznt1Chunk(
      CompressionFixturePattern(trailing_partial_unit_tail_size));
  cluster_bytes.insert(cluster_bytes.end(), tail.begin(), tail.end());

  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptCompressedUnit() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, xca_lznt1_example_decompressed_size, runs);

  return BuildCompressionImage(record, runs, MakeCorruptLznt1Chunk());
}

std::vector<BYTE> BuildFakeNtfsImageWithUnmappedCompressionUnit() {
  // Only unit 0's clusters are actually mapped; last_vcn claims two units'
  // worth.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, compression_unit_clusters}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, 2ULL * compression_unit_size, runs,
      {.last_vcn = 2ULL * compression_unit_clusters - 1});

  return BuildCompressionImage(
      record, runs, CompressionFixturePattern(compression_unit_size));
}

std::vector<BYTE> BuildFakeNtfsImageWithRealClustersAfterHole() {
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, 1}, {{}, 1}, {fragmented_compressed_data_lcn, 2}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, compression_unit_size, runs);

  // Content is irrelevant - the layout is rejected before anything is
  // decompressed - but a real LZNT1 stream keeps the fixture honest about
  // being otherwise plausible.
  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithShortDecompressedUnit() {
  // One real cluster then seven sparse: unit 0 (VCN 0..3) is compressed,
  // unit 1 (VCN 4..7) a hole. real_size covers both, so unit 0 is interior
  // and must yield a whole compression_unit_size bytes.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, 1}, {{}, 2ULL * compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, 2ULL * compression_unit_size, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(
          CompressionFixturePattern(short_decompressed_unit_size)));
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedAttrMissingCompressedSize() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, xca_lznt1_example_decompressed_size, runs,
      {.total_size = compressed_attr_truncated_total_size});

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithCompUnitSizeOutOfRange() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      comp_unit_size_out_of_range_shift, xca_lznt1_example_decompressed_size,
      runs);

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithOversizedCompressionUnit() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      oversized_comp_unit_size_shift, xca_lznt1_example_decompressed_size,
      runs);

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithMisalignedCompressedStartVcn() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, xca_lznt1_example_decompressed_size, runs,
      {.start_vcn = misaligned_compressed_start_vcn});

  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

namespace {

// Writes a malformed attribute at record[offset], as defect describes it.
void WriteTrailingDefect(FakeRecord& record, DWORD offset,
                         FakeTrailingDefect defect) {
  // A resident header never fits in this: ParseAttrs() rejects it.
  constexpr DWORD undersized_total_size = 8;

  switch (defect) {
    case FakeTrailingDefect::UndersizedHeader: {
      auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
          &record.at(offset));
      attr.header.type = AttrType::Bitmap;
      attr.header.non_resident = 0;
      attr.header.total_size = undersized_total_size;
      break;
    }
    case FakeTrailingDefect::UndersizedCompressedField: {
      auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
          &record.at(offset));
      attr.header.type = AttrType::Bitmap;
      attr.header.non_resident = 1;
      attr.comp_unit_size = compression_unit_size_shift;
      attr.header.total_size = NtfsBrowser::Attr::header_non_resident_base_size;
      break;
    }
    case FakeTrailingDefect::RejectedAttribute: {
      auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
          &record.at(offset));
      attr.header.type = AttrType::StandardInformation;
      attr.header.non_resident = 1;
      attr.header.total_size = AlignAttrSize(
          NtfsBrowser::Attr::header_non_resident_base_size + run_list_room);
      break;
    }
  }
}

}  // namespace

namespace {

// Writes a resident $EFS attribute holding "body" and returns its total_size.
DWORD WriteResidentEfsAttr(FakeRecord& record, DWORD offset,
                           std::span<const BYTE> body) {
  constexpr std::wstring_view name_value = L"$EFS";
  const std::vector<WORD> encoded_name = ToUtf16(name_value);
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::LoggedUtilityStream;
  attr.header.non_resident = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.header.name_length = gsl::narrow<BYTE>(encoded_name.size());
  attr.header.name_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = gsl::narrow<DWORD>(body.size());
  attr.attr_offset =
      gsl::narrow<WORD>(sizeof(attr) + (encoded_name.size() * sizeof(WORD)));
  attr.header.total_size = AlignAttrSize(attr.attr_offset + attr.attr_size);

  std::memcpy(&record.at(offset + attr.header.name_offset), encoded_name.data(),
              encoded_name.size() * sizeof(WORD));
  std::memcpy(&record.at(offset + attr.attr_offset), body.data(), body.size());
  return attr.header.total_size;
}

}  // namespace

std::vector<BYTE>
    BuildFakeNtfsImageWithEncryptedFile(const FakeEncryptedFile& file) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset,
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Encrypted);

  for (const FakeEncryptedStream& stream : file.streams) {
    WORD flags = stream.flagged_encrypted
                     ? NtfsBrowser::Efs::attr_flag_encrypted
                     : static_cast<WORD>(0);
    if (stream.flagged_compressed) {
      flags |= attr_flag_compressed;
    }
    offset += WriteNonResidentAttr(
        record, offset, AttrType::Data, 0, stream.real_size, stream.runs,
        {.name = stream.name, .flags = flags, .ini_size = stream.ini_size});
  }

  std::vector<FakeDataRun> efs_runs;
  if (!file.efs_stream.empty()) {
    if (file.efs_resident) {
      const DWORD efs_offset = offset;
      offset += WriteResidentEfsAttr(record, offset, file.efs_stream);
      if (file.efs_body_overruns) {
        // attr_offset + attr_size now reaches past total_size.
        constexpr DWORD overrun = 64;
        reinterpret_cast<NtfsBrowser::Attr::HeaderResident&>(
            record.at(efs_offset))
            .attr_size += overrun;
      }
    } else {
      efs_runs.push_back(
          {fake_efs_stream_lcn,
           gsl::narrow<DWORD>((file.efs_stream.size() + cluster_size - 1) /
                              cluster_size)});
      offset += WriteNonResidentAttr(
          record, offset, AttrType::LoggedUtilityStream, 0,
          file.efs_stream.size(), efs_runs, {.name = L"$EFS"});
    }
  }
  if (file.trailing_undersized_attribute) {
    WriteTrailingDefect(record, offset, FakeTrailingDefect::UndersizedHeader);
  } else {
    WriteEndOfAttributesMarker(record, offset);
  }

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  for (const FakeEncryptedStream& stream : file.streams) {
    LayRunBytes(image, stream.runs, stream.cluster_bytes);
  }
  LayRunBytes(image, efs_runs, file.efs_stream);

  return image;
}

namespace {

// Builds a root-directory replacement whose sole attribute is a RESIDENT
// $DATA carrying the EFS "encrypted" attribute-header flag. Real NTFS never
// encrypts a resident stream (EFS only ever leaves file data non-resident),
// but AttachEfsContext() must still handle a forged one.
FakeRecord MakeResidentEncryptedDataRecord() {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = NtfsBrowser::Efs::attr_flag_encrypted;
  attr.header.id = 0;
  attr.attr_size = gsl::narrow<DWORD>(resident_encrypted_data_content.size());
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  std::memcpy(&record.at(attr_offset_value + attr.attr_offset),
              resident_encrypted_data_content.data(),
              resident_encrypted_data_content.size());

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithResidentEncryptedData() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeResidentEncryptedDataRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithEncryptedDirectory() {
  const std::array<FakeIndexName, 1> root_names{
      {{encrypted_directory_names.at(0), encrypted_directory_mft_refs.at(0),
        false}}};
  const std::array<FakeIndexName, 1> block_names{
      {{encrypted_directory_names.at(1), encrypted_directory_mft_refs.at(1),
        true}}};

  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Encrypted,
      0, fake_file_record_size, runs, {}, root_names);

  return BuildCompressionImage(record, runs,
                               MakeIndexBlockContent(block_names));
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedEncryptedDirectory() {
  const std::array<FakeIndexName, 1> root_names{
      {{encrypted_directory_names.at(0), encrypted_directory_mft_refs.at(0),
        false}}};
  const std::array<FakeIndexName, 1> block_names{
      {{encrypted_directory_names.at(1), encrypted_directory_mft_refs.at(1),
        true}}};

  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed |
          NtfsBrowser::Flag::StdInfoPermission::Encrypted,
      compression_unit_size_shift, fake_file_record_size, runs, {}, root_names);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeIndexBlockContent(block_names)));
}

std::vector<BYTE> BuildFakeNtfsImageWithMinimalNonResidentData() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.start_vcn = 0;
  attr.last_vcn = 0;
  attr.comp_unit_size = 0;
  attr.real_size = 0;
  attr.alloc_size = 0;
  attr.ini_size = 0;
  // data_run_offset == total_size: the attribute is exactly its own 64-byte
  // base header, with no run-list bytes at all. Legal, and the smallest
  // total_size FileRecord<S>::ParseAttrs() may accept for a non-resident
  // attribute - the whole point of this fixture.
  attr.data_run_offset =
      static_cast<WORD>(NtfsBrowser::Attr::header_non_resident_base_size);
  attr.header.total_size = NtfsBrowser::Attr::header_non_resident_base_size;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedIndexAllocation() {
  // Two real clusters (a 1026-byte uncompressed LZNT1 chunk wrapping one
  // whole index block) plus a 2-cluster sparse pad: fewer real clusters than
  // the unit holds, so the unit is compressed.
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithCorruptCompressedIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs);

  return BuildCompressionImage(record, runs, MakeCorruptLznt1Chunk());
}

std::vector<BYTE> BuildFakeNtfsImageWithSurrogatePairNames() {
  const std::array<FakeIndexName, 2> root_names{
      {{surrogate_names.at(0), surrogate_name_mft_refs.at(0),
        surrogate_name_is_directory.at(0)},
       {surrogate_names.at(1), surrogate_name_mft_refs.at(1),
        surrogate_name_is_directory.at(1)}}};
  const std::array<FakeIndexName, 2> block_names{
      {{surrogate_names.at(2), surrogate_name_mft_refs.at(2),
        surrogate_name_is_directory.at(2)},
       {surrogate_names.at(3), surrogate_name_mft_refs.at(3),
        surrogate_name_is_directory.at(3)}}};

  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs, {}, root_names);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeIndexBlockContent(block_names)));
}

////////////////////////////////////////////////////////////////////////////
// Fuzz-corpus-only compressed $INDEX_ALLOCATION fixtures - stay on
// $INDEX_ALLOCATION, never $DATA, since that is all FuzzOnce() ReadData()s.
////////////////////////////////////////////////////////////////////////////

std::vector<BYTE>
    BuildFakeNtfsImageWithCompUnitSizeOutOfRangeIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      comp_unit_size_out_of_range_shift, fake_file_record_size, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE>
    BuildFakeNtfsImageWithOversizedCompressionUnitIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      oversized_comp_unit_size_shift, fake_file_record_size, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithMisalignedStartVcnIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs,
      {.start_vcn = misaligned_compressed_start_vcn});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE> BuildFakeNtfsImageWithMissingCompressedSizeIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2},
                                      {{}, compression_unit_clusters - 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs,
      {.total_size = compressed_attr_truncated_total_size});

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(MakeCompressedIndexBlockContent()));
}

std::vector<BYTE>
    BuildFakeNtfsImageWithUnmappedCompressionUnitIndexAllocation() {
  // Only 2 of unit 0's 4 clusters are actually mapped by the run list, but
  // last_vcn (forced to 2 via the override) claims a 3-cluster attribute -
  // LeadingRealClusters() walks the one real run, reaches vcn 2, then runs
  // out of runs with vcn(2) != unitEnd(3).
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs,
      {.last_vcn = 2});

  return BuildCompressionImage(
      record, runs, CompressionFixturePattern(compression_unit_size));
}

std::vector<BYTE> BuildFakeNtfsImageWithRealClustersAfterHoleIndexAllocation() {
  // real, hole, real - within a single compression unit, no encoding this
  // library understands produces real clusters after a hole, so
  // LeadingRealClusters() must reject it before ParseIndexBlock() ever tries
  // to decompress anything.
  const std::vector<FakeDataRun> runs{
      {compressed_data_lcn, 1}, {{}, 1}, {fragmented_compressed_data_lcn, 2}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, compression_unit_size, runs);

  // Content is irrelevant - the layout is rejected before anything is
  // decompressed - but real LZNT1-looking bytes keep the fixture honest
  // about being otherwise plausible.
  const std::vector<BYTE> cluster_bytes(xca_lznt1_example_compressed.begin(),
                                        xca_lznt1_example_compressed.end());
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithSparseCompressionUnitIndexAllocation() {
  // A pure hole: LeadingRealClusters() returns 0, so GetCompressionUnit()
  // takes its "sparse" branch - the fixture's own point, independent of
  // ParseIndexBlock()'s later, separate magic-check failure on the result.
  const std::vector<FakeDataRun> runs{{{}, compression_unit_clusters}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed |
          NtfsBrowser::Flag::StdInfoPermission::Sparse,
      compression_unit_size_shift, compression_unit_size, runs);

  return BuildCompressionImage(record, runs, {});
}

std::vector<BYTE> BuildFakeNtfsImageWithShortDecompressedUnitIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, compression_unit_size, runs);

  return BuildCompressionImage(
      record, runs,
      MakeUncompressedLznt1Chunk(
          CompressionFixturePattern(short_decompressed_unit_size)));
}

namespace {

// LCN whose product with this fixture's cluster size overflows a signed
// LONGLONG inside ReadClusters()'s gsl::narrow<LONGLONG>() call - same
// magnitude as huge_mft_lcn, applied to a data run's LCN instead.
constexpr ULONGLONG overflowing_lcn = 1ULL << 53U;

// Directory record shaped like MakeIndexAllocationDirRecord(), but with a
// single hand-encoded run (8-byte LCN offset) at overflowing_lcn, so it
// reaches ReadClusters()'s narrowing failure from GetCompressionUnit().
FakeRecord
    MakeIndexAllocationDirRecordWithOverflowingLcn(DWORD real_run_clusters) {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset,
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed);

  // $INDEX_ROOT - identical nameless SUBNODE-only entry as
  // MakeIndexAllocationDirRecord().
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = fake_file_record_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode |
                      NtfsBrowser::Flag::IndexEntry::Last;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  auto& sub_node_vcn = SubNodeVcnSlot(first_entry);
  sub_node_vcn = 0;

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  // $INDEX_ALLOCATION: compressed, comp_unit_size ==
  // compression_unit_size_shift.
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = AttrType::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = attr_flag_compressed;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = compression_unit_clusters - 1;
  alloc_attr.comp_unit_size = compression_unit_size_shift;
  alloc_attr.alloc_size = ULONGLONG{compression_unit_clusters} * cluster_size;
  alloc_attr.real_size = fake_file_record_size;
  alloc_attr.ini_size = alloc_attr.real_size;

  const auto header_size = static_cast<WORD>(
      sizeof(alloc_attr) + NtfsBrowser::Attr::compressed_size_field_size);
  alloc_attr.data_run_offset = header_size;

  const ULONGLONG compressed_size =
      static_cast<ULONGLONG>(real_run_clusters) * cluster_size;
  std::memcpy(&record.at(offset + sizeof(alloc_attr)), &compressed_size,
              sizeof(compressed_size));

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + header_size);
  DWORD run_len = 0;
  // Real run: header 0x81 (8-byte LCN-offset field), an 8-byte LE delta of
  // overflowing_lcn, covering realRunClusters clusters.
  gsl::at(data_run, run_len++) = run_header8_lcn_bytes;
  gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(real_run_clusters);
  {
    const auto delta = static_cast<LONGLONG>(overflowing_lcn);
    std::memcpy(&gsl::at(data_run, run_len), &delta, sizeof(delta));
    run_len += sizeof(delta);
  }
  if (real_run_clusters < compression_unit_clusters) {
    // Sparse run padding the unit out to a whole compression unit (header
    // byte 0x01: 1-byte length field, 0-byte offset field).
    gsl::at(data_run, run_len++) = 0x01;
    gsl::at(data_run, run_len++) =
        gsl::narrow<BYTE>(compression_unit_clusters - real_run_clusters);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(header_size + run_len);

  offset += alloc_attr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithStoredCompressionUnitBadLcn() {
  // All 4 clusters real (no sparse) - the "stored" branch.
  const FakeRecord record =
      MakeIndexAllocationDirRecordWithOverflowingLcn(compression_unit_clusters);

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithCompressedCompressionUnitBadLcn() {
  // 1 real cluster (at the overflowing LCN) + 3 sparse - the "compressed"
  // branch (realClusters < unitClusters).
  const FakeRecord record = MakeIndexAllocationDirRecordWithOverflowingLcn(1);

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());
  return image;
}

////////////////////////////////////////////////////////////////////////////
// LZNT1 decompressor rejection-path fixtures (src/lznt1/decompress.cpp):
// each a single compression unit whose real cluster(s) hold a hand-crafted,
// malformed LZNT1 byte stream.
////////////////////////////////////////////////////////////////////////////

std::vector<BYTE> BuildFakeNtfsImageWithLznt1InvalidSignatureIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs);

  // Chunk header 0x1002: bit 15 clear (not the end-of-buffer 0x0000 marker),
  // bits 14-12 == 1 != the mandatory signature 3.
  const std::vector<BYTE> cluster_bytes{0x02, 0x10};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1ChunkExceedsSrcBoundsIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs);

  // Chunk header 0xBFFF: valid, declares a 4096-byte payload - far more than
  // the ~1022 bytes actually available in the one real cluster.
  const std::vector<BYTE> cluster_bytes{0xFF, 0xBF};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1UncompressedChunkExceedsDestIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs);

  // Chunk 1 decompresses to just short of the 4096-byte unit; chunk 2
  // (uncompressed, declared payload 10) has too little dest buffer left.
  const std::vector<BYTE> cluster_bytes{0x03, 0xB0, 0x02, 0xAA,
                                        0xF4, 0x0F, 0x09, 0x30};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithLznt1ChunkOver4096IndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs);

  // Output reaches exactly 4096 bytes, then one more trailing byte forces a
  // 3rd data element whose own bounds check must reject it.
  const std::vector<BYTE> cluster_bytes{0x04, 0xB0, 0x02, 0xAA,
                                        0xFC, 0x0F, 0x00};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1LiteralExceedsDestIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs);

  // Chunk 1 fills the dest buffer to exactly 4096 bytes; chunk 2's first
  // element is a literal, rejected for writing past a buffer already full.
  const std::vector<BYTE> cluster_bytes{0x03, 0xB0, 0x02, 0xAA, 0xFC,
                                        0x0F, 0x01, 0xB0, 0x00, 0x00};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithLznt1TruncatedWordIndexAllocation() {
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1},
                                      {{}, compression_unit_clusters - 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      compression_unit_size_shift, fake_file_record_size, runs);

  // Chunk header 0xB001 (payload 2): a flag byte (0x01 - a compressed word)
  // followed by only ONE more byte, not the two a compressed word needs.
  const std::vector<BYTE> cluster_bytes{0x01, 0xB0, 0x01, 0x00};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE>
    BuildFakeNtfsImageWithLznt1BackreferenceExceedsDestIndexAllocation() {
  // comp_unit_size == 1 (2048-byte unit), deliberately smaller than
  // chunk_size, so a legal-looking back-reference can still be rejected
  // purely for exceeding this smaller unit's own dest buffer.
  const std::vector<FakeDataRun> runs{{compressed_data_lcn, 1}, {{}, 1}};

  const FakeRecord record = MakeIndexAllocationDirRecord(
      NtfsBrowser::Flag::StdInfoPermission::Archive |
          NtfsBrowser::Flag::StdInfoPermission::Compressed,
      1, fake_file_record_size, runs);

  // Back-reference (length 2048) fits the per-chunk cap but exceeds this
  // unit's own 2048-byte dest buffer.
  const std::vector<BYTE> cluster_bytes{0x03, 0xB0, 0x02, 0xAA, 0xFD, 0x07};
  return BuildCompressionImage(record, runs, cluster_bytes);
}

std::vector<BYTE> BuildFakeNtfsImageWithBadDataRun() {
  // Two real 1-cluster runs (VCN 0, then VCN 1 if both were accepted), but
  // last_vcn is forged to 0: the second run's own last_vcn (1) then exceeds
  // it, mid-list.
  const std::vector<FakeDataRun> runs{{bad_data_run_first_lcn, 1},
                                      {bad_data_run_second_lcn, 1}};

  const FakeRecord record =
      MakeNonResidentDataRecord(NtfsBrowser::Flag::StdInfoPermission::Archive,
                                0, cluster_size, runs, {.last_vcn = 0});

  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  std::memcpy(&image.at(root_offset), record.data(), record.size());
  return image;
}

namespace {

// LCN where BuildFakeNtfsImageWithBadIndexBlockEntry() writes its two real,
// sibling index blocks (VCN 0 and VCN 1), clear of every other fixture's
// placement in this file.
constexpr DWORD bad_index_block_lcn = 220;

// Builds a root-directory replacement whose $INDEX_ROOT holds two real
// subnode-pointer entries (VCN 0, VCN 1): both are true B+ tree children,
// reached by the normal walk with no recovery needed.
FakeRecord MakeBadIndexBlockEntryRootRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  DWORD offset = attr_offset_value;

  // $INDEX_ROOT
  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = cluster_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  // Entry 1: nameless, non-terminal subnode pointer to VCN 0 (the damaged
  // block).
  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = 0;
  first_entry.mft_sn = 0;
  first_entry.stream_size = 0;
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode;
  first_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  SubNodeVcnSlot(first_entry) = 0;

  // Entry 2: nameless, terminal subnode pointer to VCN 1 (the good block).
  auto& second_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(
          body, gsl::narrow<gsl::index>(sizeof(NtfsBrowser::Attr::IndexRoot) +
                                        first_entry.size)));
  second_entry.mft_index = 0;
  second_entry.mft_sn = 0;
  second_entry.stream_size = 0;
  second_entry.flags = NtfsBrowser::Flag::IndexEntry::SubNode |
                       NtfsBrowser::Flag::IndexEntry::Last;
  second_entry.size = static_cast<WORD>(AlignAttrSize(
      offsetof(NtfsBrowser::Data::IndexEntry, stream) + sizeof(ULONGLONG)));
  SubNodeVcnSlot(second_entry) = 1;

  root.total_entry_size = first_entry.size + second_entry.size;
  root.alloc_entry_size = root.total_entry_size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      root.total_entry_size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;

  // $INDEX_ALLOCATION: 2 contiguous blocks at bad_index_block_lcn.
  auto& alloc_attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderNonResident*>(
      &record.at(offset));
  alloc_attr.header.type = AttrType::IndexAllocation;
  alloc_attr.header.non_resident = 1;
  alloc_attr.header.name_length = 0;
  alloc_attr.header.flags = 0;
  alloc_attr.header.id = 0;
  alloc_attr.start_vcn = 0;
  alloc_attr.last_vcn = 1;
  alloc_attr.data_run_offset = static_cast<WORD>(sizeof(alloc_attr));
  alloc_attr.comp_unit_size = 0;
  alloc_attr.real_size = 2ULL * cluster_size;
  alloc_attr.alloc_size = alloc_attr.real_size;
  alloc_attr.ini_size = alloc_attr.real_size;

  const std::span<BYTE> data_run =
      std::span<BYTE>(record).subspan(offset + alloc_attr.data_run_offset);
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = 2;  // 2 clusters
  {
    const DWORD lcn = bad_index_block_lcn;
    std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
    run_len += sizeof(lcn);
  }
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list

  alloc_attr.header.total_size = AlignAttrSize(sizeof(alloc_attr) + run_len);
  offset += alloc_attr.header.total_size;

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

// Writes one real (named, non-subnode) leaf entry at entryPtr and returns
// its size.
WORD WriteBadIndexBlockLeafEntry(BYTE* entry_ptr, ULONGLONG mft_ref,
                                 ULONGLONG parent_ref, std::wstring_view name,
                                 bool last) {
  const auto name_length = gsl::narrow<BYTE>(name.size());
  auto& index_entry =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(entry_ptr);
  index_entry.mft_index = mft_ref;
  index_entry.mft_sn = 1;

  auto& filename =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&index_entry.stream);
  filename.parent_ref = parent_ref;
  filename.flags = NtfsBrowser::Flag::Filename::None;
  filename.name_length = name_length;
  filename.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;
  for (BYTE i = 0; i < name_length; i++) {
    // nameLength is name.size(), so i < name.size() by the loop condition.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    filename.name[i] = gsl::narrow<WORD>(name[i]);
  }

  index_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&filename.name[name_length]) -
                        reinterpret_cast<BYTE*>(&filename));
  index_entry.flags = last ? NtfsBrowser::Flag::IndexEntry::Last
                           : NtfsBrowser::Flag::IndexEntry{};
  index_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &index_entry.stream - reinterpret_cast<BYTE*>(&index_entry) +
      index_entry.stream_size));
  return index_entry.size;
}

// Returns the bytes of bad index block "vcn" that follow its header.
std::span<BYTE> BadIndexBlockBody(std::vector<BYTE>& image, DWORD vcn) {
  const size_t block_offset =
      (static_cast<size_t>(bad_index_block_lcn) + vcn) * cluster_size;
  return std::span<BYTE>(image).subspan(
      block_offset + sizeof(NtfsBrowser::Data::IndexBlock),
      cluster_size - sizeof(NtfsBrowser::Data::IndexBlock));
}

// Writes the block header (magic, fixup, entry_offset) shared by both of
// BuildFakeNtfsImageWithBadIndexBlockEntry()'s blocks, at VCN vcn (relative
// to bad_index_block_lcn).
NtfsBrowser::Data::IndexBlock&
    WriteBadIndexBlockHeader(std::vector<BYTE>& image, DWORD vcn) {
  const size_t block_offset =
      (static_cast<size_t>(bad_index_block_lcn) + vcn) * cluster_size;
  const std::span<BYTE> block_start =
      std::span<BYTE>(image).subspan(block_offset);
  auto& block =
      *reinterpret_cast<NtfsBrowser::Data::IndexBlock*>(block_start.data());
  std::memset(&block, 0, sizeof(block));
  block.magic = index_block_magic;
  // Points at the block's own last 6 bytes, so PatchUS() succeeds trivially
  // without a real fixup array.
  block.offset_of_us = static_cast<WORD>(cluster_size - us_slot_size);
  block.size_of_us = 3;
  block.vcn = vcn;
  block.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(block_start, sizeof(NtfsBrowser::Data::IndexBlock))) -
      reinterpret_cast<BYTE*>(&block.entry_offset));
  block.not_leaf = 0;
  return block;
}

// VCN 0: one real entry ("First"), then an entry whose declared size
// overruns the block - AttrIndexAlloc::ParseIndexBlock() must reject the
// whole block when strict, but keep "First" (parsed before the bad entry)
// when recovering.
void WriteBadIndexBlockDamagedBlock(std::vector<BYTE>& image,
                                    ULONGLONG parent_ref) {
  NtfsBrowser::Data::IndexBlock& block = WriteBadIndexBlockHeader(image, 0);
  const std::span<BYTE> body = BadIndexBlockBody(image, 0);

  const WORD size_a = WriteBadIndexBlockLeafEntry(
      body.data(), bad_index_block_first_mft_ref, parent_ref,
      bad_index_block_first_name, /*last=*/false);

  auto& entry_b =
      *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(&gsl::at(body, size_a));
  entry_b.mft_index = 0;
  entry_b.mft_sn = 0;
  entry_b.stream_size = 0;
  entry_b.flags = NtfsBrowser::Flag::IndexEntry::Last;
  // Declares far more than the block actually has left after entry A:
  // ParseIndexBlock()'s "index entry exceeds block bounds" check.
  entry_b.size = static_cast<WORD>(cluster_size);

  block.total_entry_size = static_cast<DWORD>(size_a) + entry_b.size;
  block.alloc_entry_size = block.total_entry_size;
}

// VCN 1: one well-formed, terminal leaf entry ("Good") - a sibling,
// unaffected by the other block's defect.
void WriteBadIndexBlockGoodBlock(std::vector<BYTE>& image,
                                 ULONGLONG parent_ref) {
  NtfsBrowser::Data::IndexBlock& block = WriteBadIndexBlockHeader(image, 1);
  const std::span<BYTE> body = BadIndexBlockBody(image, 1);

  const WORD size_good =
      WriteBadIndexBlockLeafEntry(body.data(), bad_index_block_good_mft_ref,
                                  parent_ref, bad_index_block_good_name,
                                  /*last=*/true);

  block.total_entry_size = size_good;
  block.alloc_entry_size = size_good;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithBadIndexBlockEntry() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeBadIndexBlockEntryRootRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  const size_t blocks_offset =
      static_cast<size_t>(bad_index_block_lcn) * cluster_size;
  const size_t blocks_bytes = 2ULL * cluster_size;
  // FullCache always reads a whole 64KiB-aligned block, so the image must
  // extend past the blocks' real end or its last read fails outright.
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t blocks_end = blocks_offset + blocks_bytes;
  const size_t aligned_blocks_end =
      ((blocks_end + full_cache_read_block_size - 1) /
       full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_blocks_end) {
    image.resize(aligned_blocks_end, 0);
  }

  const ULONGLONG root_ref = MakeFileReference(
      static_cast<ULONGLONG>(MftIdx::Root), root_sequence_number);

  WriteBadIndexBlockDamagedBlock(image, root_ref);
  WriteBadIndexBlockGoodBlock(image, root_ref);

  return image;
}

namespace {

// Builds a root-directory replacement whose $INDEX_ROOT holds a single,
// terminal entry: a real 3-character name is written on disk, but
// name_length claims far more than that.
FakeRecord MakeMalformedIndexEntryFilenameRootRecord() {
  FakeRecord record = MakeRecordHeader(attr_offset_value,
                                       NtfsBrowser::Flag::FileRecord::InUse |
                                           NtfsBrowser::Flag::FileRecord::Dir);

  DWORD offset = attr_offset_value;

  auto& root_attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  root_attr.header.type = AttrType::IndexRoot;
  root_attr.header.non_resident = 0;
  root_attr.header.name_length = 0;
  root_attr.header.flags = 0;
  root_attr.header.id = 0;
  root_attr.attr_offset = static_cast<WORD>(sizeof(root_attr));

  const std::span<BYTE> body =
      std::span<BYTE>(record).subspan(offset + root_attr.attr_offset);
  auto& root = *reinterpret_cast<NtfsBrowser::Attr::IndexRoot*>(body.data());
  root.attr_type = AttrType::FileName;
  root.coll_rule = 0;
  root.ib_size = cluster_size;
  root.clusters_per_ib = 1;
  root.entry_offset = gsl::narrow<DWORD>(
      (&gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot))) -
      reinterpret_cast<BYTE*>(&root.entry_offset));

  auto& first_entry = *reinterpret_cast<NtfsBrowser::Data::IndexEntry*>(
      &gsl::at(body, sizeof(NtfsBrowser::Attr::IndexRoot)));
  first_entry.mft_index = malformed_index_entry_mft_ref;
  first_entry.mft_sn = 1;

  auto& fn1 =
      *reinterpret_cast<NtfsBrowser::Attr::Filename*>(&first_entry.stream);
  fn1.parent_ref = MakeFileReference(static_cast<ULONGLONG>(MftIdx::Root),
                                     root_sequence_number);
  fn1.flags = NtfsBrowser::Flag::Filename::None;
  fn1.name_space = NtfsBrowser::Flag::FilenameNamespace::Win32;

  // The real, on-disk name is short; stream_size (and hence e1.size) is
  // sized to it, not to the forged name_length below.
  constexpr std::wstring_view real_name = L"Bad";
  constexpr BYTE real_name_length = 3;
  for (BYTE i = 0; i < real_name_length; i++) {
    // i is below name_length, the length of the name.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    fn1.name[i] = gsl::narrow<WORD>(real_name[i]);
  }
  // Claims far more characters than the entry has room for.
  fn1.name_length = overlong_name_length;

  first_entry.stream_size =
      gsl::narrow<WORD>(reinterpret_cast<BYTE*>(&fn1.name[real_name_length]) -
                        reinterpret_cast<BYTE*>(&fn1));
  first_entry.flags = NtfsBrowser::Flag::IndexEntry::Last;
  first_entry.size = gsl::narrow<WORD>(AlignAttrSize(
      &first_entry.stream - reinterpret_cast<BYTE*>(&first_entry) +
      first_entry.stream_size));

  root.total_entry_size = first_entry.size;
  root.alloc_entry_size = first_entry.size;
  root.flags = 0;

  root_attr.attr_size =
      static_cast<DWORD>(sizeof(NtfsBrowser::Attr::IndexRoot)) +
      first_entry.size;
  root_attr.header.total_size =
      AlignAttrSize(sizeof(root_attr) + root_attr.attr_size);

  offset += root_attr.header.total_size;
  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithMalformedIndexEntryFilename() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeMalformedIndexEntryFilenameRootRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());
  return image;
}

namespace {

// Builds a root-directory replacement whose sole resident $DATA attribute's
// total_size reaches exactly to the end of the file record: no bytes are
// left for a trailing AttrType::ALL end-of-attributes marker. The trailing
// bytes stay zero (never written), matching offset_of_us's self-consistent
// fixup trick.
FakeRecord MakeNoEndMarkerRecord() {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);

  auto& attr = *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = AttrType::Data;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));

  // Fills the record exactly to its own end, leaving no room for a marker.
  const DWORD total_size = fake_file_record_size - attr_offset_value;
  attr.attr_size = total_size - static_cast<DWORD>(sizeof(attr));
  attr.header.total_size = total_size;

  return record;
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithNoEndMarker() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;
  const size_t root_offset =
      mft_addr + static_cast<size_t>(fake_file_record_size) *
                     static_cast<size_t>(MftIdx::Root);
  const FakeRecord record = MakeNoEndMarkerRecord();
  std::memcpy(&image.at(root_offset), record.data(), record.size());

  return image;
}

std::filesystem::path WriteFakeNtfsImage() {
  const std::vector<BYTE> image = BuildFakeNtfsImage();

  std::random_device random_device;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      (L"ntfsbrowser-fake-volume-" + std::to_wstring(random_device()) +
       L".img");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(image.data()),
            gsl::narrow<std::streamsize>(image.size()));

  return path;
}

namespace {

// Fixed on-disk size of a nameless $ATTRIBUTE_LIST entry.
constexpr WORD list_entry_size = static_cast<WORD>(
    AlignAttrSize(NtfsBrowser::Attr::attribute_list_entry_header_size));

// LCNs the lifetime fixtures use for cluster data: past the $MFT records
// (LCN 1 onwards) and distinct, so no two streams share a cluster.
constexpr DWORD lifetime_base_data_lcn = 50;
constexpr DWORD lifetime_ext_data_lcn = 51;
constexpr DWORD lifetime_list_lcn = 52;

// Writes one nameless $ATTRIBUTE_LIST entry at dest: type, the record it
// points to (with the sequence number it claims for it), and the record_size
// to declare (0 is the forged value).
void WriteListEntry(BYTE* dest, AttrType type, ULONGLONG record,
                    WORD record_size, WORD sequence = 0) {
  auto& entry = *reinterpret_cast<NtfsBrowser::Attr::AttributeList*>(dest);
  entry.attr_type = type;
  entry.record_size = record_size;
  entry.name_length = 0;
  entry.name_offset = 0;
  entry.start_vcn = 0;
  entry.base_ref.segment_number = record;
  entry.base_ref.sequence_number = sequence;
  entry.attr_id = 0;
}

// Writes a resident $ATTRIBUTE_LIST holding "entryCount" entries, all for
// $DATA in the lifetime extension record, and returns its total_size. The
// entry at index "zeroSizeIndex", if any, declares a record_size of 0. Every
// entry claims "entrySequence" for the extension record.
DWORD WriteLifetimeResidentList(FakeRecord& record, DWORD offset,
                                size_t entry_count,
                                std::optional<size_t> zero_size_index = {},
                                WORD entry_sequence = 0) {
  auto& attr =
      *reinterpret_cast<NtfsBrowser::Attr::HeaderResident*>(&record.at(offset));
  attr.header.type = AttrType::AttributeList;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  attr.attr_size = gsl::narrow<DWORD>(list_entry_size * entry_count);
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  for (size_t i = 0; i < entry_count; i++) {
    WriteListEntry(
        &record.at(offset + attr.attr_offset + (i * list_entry_size)),
        AttrType::Data, attr_list_lifetime_ext_idx,
        zero_size_index == i ? static_cast<WORD>(0) : list_entry_size,
        entry_sequence);
  }
  return attr.header.total_size;
}

// The extension link of an ordinary extension of attr_list_lifetime_base_idx.
constexpr FakeExtensionLink ordinary_lifetime_link{
    .base_ref = attr_list_lifetime_base_idx};

// Extension record holding one resident $DATA of
// attr_list_lifetime_data_content.
FakeRecord MakeLifetimeResidentDataExtensionRecord(
    const FakeExtensionLink& link = ordinary_lifetime_link) {
  FakeRecord record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  SetRecordLink(record, link.record_sequence, link.base_ref);

  DWORD offset = attr_offset_value;
  const DWORD data_offset = offset;
  offset += WriteResidentDataAttr(record, offset,
                                  attr_list_lifetime_data_content.size());
  std::memcpy(
      &record.at(data_offset + sizeof(NtfsBrowser::Attr::HeaderResident)),
      attr_list_lifetime_data_content.data(),
      attr_list_lifetime_data_content.size());

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE>
    BuildFakeNtfsImageWithAttributeListImportThenZeroRecordSize() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  FakeRecord base =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  DWORD offset = attr_offset_value;
  offset += WriteLifetimeResidentList(base, offset, 2, 1);
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, mft_addr, attr_list_lifetime_base_idx, base);
  PutMftRecord(image, mft_addr, attr_list_lifetime_ext_idx,
               MakeLifetimeResidentDataExtensionRecord());
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSplitAttributeListAttribute() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  FakeRecord base =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  DWORD offset = attr_offset_value;
  // VCN 0-0: empty.
  offset +=
      WriteNonResidentAttr(base, offset, AttrType::AttributeList, 0, 0, {});
  // VCN 1-1: one entry, stored in its own cluster.
  const std::vector<FakeDataRun> runs{{lifetime_list_lcn, 1}};
  offset += WriteNonResidentAttr(base, offset, AttrType::AttributeList, 0,
                                 list_entry_size, runs, {.start_vcn = 1});
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, mft_addr, attr_list_lifetime_base_idx, base);
  PutMftRecord(image, mft_addr, attr_list_lifetime_ext_idx,
               MakeLifetimeResidentDataExtensionRecord());

  std::vector<BYTE> entry(list_entry_size, 0);
  WriteListEntry(entry.data(), AttrType::Data, attr_list_lifetime_ext_idx,
                 list_entry_size);
  LayRunBytes(image, runs, entry);
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSplitDataAndTrailingDefect(
    FakeTrailingDefect defect) {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  const std::vector<FakeDataRun> base_runs{{lifetime_base_data_lcn, 1}};
  const std::vector<FakeDataRun> ext_runs{{lifetime_ext_data_lcn, 1}};

  FakeRecord base =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  DWORD offset = attr_offset_value;
  offset += WriteLifetimeResidentList(base, offset, 1);
  offset += WriteNonResidentAttr(base, offset, AttrType::Data, 0, cluster_size,
                                 base_runs);
  WriteTrailingDefect(base, offset, defect);

  FakeRecord ext =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  SetRecordLink(ext, 0, attr_list_lifetime_base_idx);
  DWORD ext_offset = attr_offset_value;
  ext_offset += WriteNonResidentAttr(ext, ext_offset, AttrType::Data, 0, 0,
                                     ext_runs, {.start_vcn = 1});
  WriteEndOfAttributesMarker(ext, ext_offset);

  PutMftRecord(image, mft_addr, attr_list_lifetime_base_idx, base);
  PutMftRecord(image, mft_addr, attr_list_lifetime_ext_idx, ext);
  LayRunBytes(image, base_runs, {});
  LayRunBytes(image, ext_runs, {});
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithExtensionLink(FakeExtensionLink link) {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  FakeRecord base =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  DWORD offset = attr_offset_value;
  offset += WriteLifetimeResidentList(base, offset, 1, {}, link.entry_sequence);
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, mft_addr, attr_list_lifetime_base_idx, base);
  PutMftRecord(image, mft_addr, attr_list_lifetime_ext_idx,
               MakeLifetimeResidentDataExtensionRecord(link));
  return image;
}

std::vector<BYTE>
    BuildFakeNtfsImageWithMftDataSplitLink(FakeExtensionLink link) {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  const std::array<std::pair<ULONGLONG, ULONGLONG>, 1> continuations{
      {{mft_data_split_ext_idx, mft_data_split_target_idx}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(MftIdx::Mft),
               MakeMftRecordWithDataContinuations(continuations, 0,
                                                  link.entry_sequence));
  PutMftRecord(image, mft_addr, mft_data_split_ext_idx,
               MakeMftDataContinuationExtensionRecord(mft_data_split_target_idx,
                                                      mft_data_split_lcn, 1,
                                                      link.record_sequence));

  FakeRecord target_record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  WriteEndOfAttributesMarker(target_record, attr_offset_value);
  PutRecordAt(image, static_cast<size_t>(mft_data_split_lcn) * cluster_size,
              target_record);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMftDataTwoExtentsInOneRecord() {
  std::vector<BYTE> image = BuildFakeNtfsImage();
  const DWORD mft_addr = static_cast<DWORD>(mft_lcn) * cluster_size;

  const std::array<std::pair<ULONGLONG, ULONGLONG>, 2> continuations{
      {{mft_two_extents_ext_idx, mft_two_extents_first_vcn},
       {mft_two_extents_ext_idx, mft_two_extents_second_vcn}}};
  PutMftRecord(image, mft_addr, static_cast<ULONGLONG>(MftIdx::Mft),
               MakeMftRecordWithDataContinuations(continuations));

  const std::vector<FakeDataRun> first_runs{{mft_two_extents_first_lcn, 1}};
  const std::vector<FakeDataRun> second_runs{{mft_two_extents_second_lcn, 1}};
  FakeRecord ext =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  SetRecordLink(ext, 0, static_cast<ULONGLONG>(MftIdx::Mft));
  DWORD ext_offset = attr_offset_value;
  ext_offset += WriteNonResidentAttr(ext, ext_offset, AttrType::Data, 0,
                                     fake_file_record_size, first_runs,
                                     {.start_vcn = mft_two_extents_first_vcn});
  ext_offset += WriteNonResidentAttr(ext, ext_offset, AttrType::Data, 0,
                                     fake_file_record_size, second_runs,
                                     {.start_vcn = mft_two_extents_second_vcn});
  WriteEndOfAttributesMarker(ext, ext_offset);
  PutMftRecord(image, mft_addr, mft_two_extents_ext_idx, ext);

  FakeRecord target_record =
      MakeRecordHeader(attr_offset_value, NtfsBrowser::Flag::FileRecord::InUse);
  WriteEndOfAttributesMarker(target_record, attr_offset_value);
  PutRecordAt(image,
              static_cast<size_t>(mft_two_extents_first_lcn) * cluster_size,
              target_record);
  PutRecordAt(image,
              static_cast<size_t>(mft_two_extents_second_lcn) * cluster_size,
              target_record);

  return image;
}

namespace {

// Appends one run to "runs": a 1-cluster run whose LCN offset field is the
// 8-byte value "delta".
void AppendEightByteLcnRun(std::vector<BYTE>& runs, LONGLONG delta) {
  runs.push_back(run_header8_lcn_bytes);
  runs.push_back(1);
  const size_t position = runs.size();
  runs.resize(position + sizeof(delta));
  std::memcpy(&runs.at(position), &delta, sizeof(delta));
}

// Same volume as BuildFakeNtfsImage(), with the root record (#5) replaced by
// one whose stream, of "host" kind, has the hand-encoded run list "runs".
// It covers "clusters" 1-cluster runs. No cluster is laid down: a run is
// never expected to be read successfully.
std::vector<BYTE> BuildImageWithRawRuns(FakeRunHost host,
                                        std::vector<BYTE> runs,
                                        DWORD clusters) {
  runs.push_back(0x00);  // terminate the run list

  const std::vector<FakeDataRun> placeholder{{{}, clusters}};
  const ULONGLONG real_size = static_cast<ULONGLONG>(clusters) * cluster_size;
  const FakeNonResidentOverrides overrides{.raw_runs = std::move(runs)};
  const auto permission = NtfsBrowser::Flag::StdInfoPermission::Archive;

  const FakeRecord record =
      (host == FakeRunHost::Data)
          ? MakeNonResidentDataRecord(permission, 0, real_size, placeholder,
                                      overrides)
          : MakeIndexAllocationDirRecord(permission, 0, real_size, placeholder,
                                         overrides);
  return BuildCompressionImage(record, {}, {});
}

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithWrappingLcn(FakeRunHost host) {
  std::vector<BYTE> runs;
  AppendEightByteLcnRun(runs, static_cast<LONGLONG>(wrapping_lcn));
  return BuildImageWithRawRuns(host, std::move(runs), 1);
}

std::vector<BYTE> BuildFakeNtfsImageWithOverflowingLcnSum(FakeRunHost host) {
  std::vector<BYTE> runs;
  AppendEightByteLcnRun(runs, std::numeric_limits<LONGLONG>::max());
  AppendEightByteLcnRun(runs, std::numeric_limits<LONGLONG>::max());
  return BuildImageWithRawRuns(host, std::move(runs), 2);
}

}  // namespace NtfsBrowserTests
