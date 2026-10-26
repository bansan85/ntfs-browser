#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include <gsl/narrow>
#include <gsl/util>

#include <ntfs-browser/namespace.h>

#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/filename-flag.h"
#include "data/filename.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/index-entry.h"
#include "data/standard-information.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "ntfs-browser/attr/header-common.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// Points the fixup slot at the record's own last bytes, so PatchUS()
// succeeds without a real fixup array.
constexpr WORD offset_of_us_value = fake_file_record_size - us_slot_size;

static_assert(attr_offset_value +
                      sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident) +
                      run_list_room <
                  offset_of_us_value,
              "attribute data must not reach into the fixup slot");

}  // namespace

// Returns the sub-node VCN slot: the last 8 bytes of the index entry `e`.
ULONGLONG&
    SubNodeVcnSlot(NTFS_BROWSER_NAMESPACE::Data::IndexEntry& index_entry) {
  const std::span<BYTE> raw(reinterpret_cast<BYTE*>(&index_entry),
                            index_entry.size);
  return *reinterpret_cast<ULONGLONG*>(
      &gsl::at(raw, gsl::narrow<gsl::index>(raw.size() - sizeof(ULONGLONG))));
}

// Builds a bare file-record header with the given attribute offset and
// flags.
FakeRecord MakeRecordHeader(WORD offset_of_attr,
                            NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag flags,
                            WORD sequence) {
  FakeRecord record{};

  EditFileRecordHeader(record, [&](FileRecordHeader& header) {
    header.magic = FileRecordHeader::file_record_magic;
    header.offset_of_us = offset_of_us_value;
    header.size_of_us = 3;
    header.seq_no = sequence;
    header.offset_of_attr = offset_of_attr;
    header.flags = flags;
  });

  return record;
}

FakeRecord MakeInUseFileRecord(WORD sequence) {
  return MakeRecordHeader(attr_offset_value,
                          NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse,
                          sequence);
}

FakeRecord MakeInUseDirectoryRecord(WORD sequence) {
  return MakeRecordHeader(attr_offset_value,
                          NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::InUse |
                              NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir,
                          sequence);
}

// Makes record an extension record: its own sequence number, and the file
// reference of the base record it belongs to.
void SetRecordLink(FakeRecord& record, WORD sequence, ULONGLONG base_ref) {
  EditFileRecordHeader(record, [&](FileRecordHeader& header) {
    header.seq_no = sequence;
    header.ref_to_base = base_ref;
  });
}

// Writes the Attr::Type::All end-of-attributes marker at offset.
void WriteEndOfAttributesMarker(FakeRecord& record, DWORD offset) {
  const auto marker = static_cast<DWORD>(Attr::Type::All);
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

void PadToFullCacheRead(std::vector<BYTE>& image, size_t end) {
  constexpr size_t full_cache_read_block_size = size_t{64} * 1024;
  const size_t aligned_end =
      ((end + full_cache_read_block_size - 1) / full_cache_read_block_size) *
      full_cache_read_block_size;
  if (image.size() < aligned_end) {
    image.resize(aligned_end, 0);
  }
}

// Writes record at byteOffset, growing image to the next 64 KiB boundary -
// FullCache always reads a whole 64 KiB block, so a short image short-reads.
void PutRecordAt(std::vector<BYTE>& image, size_t byte_offset,
                 const FakeRecord& record) {
  PadToFullCacheRead(image, byte_offset + record.size());
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

void PutMftRecord(std::vector<BYTE>& image, ULONGLONG idx,
                  const FakeRecord& record) {
  PutMftRecord(image, mft_address, idx, record);
}

void PutRootRecord(std::vector<BYTE>& image, const FakeRecord& record) {
  PutMftRecord(image, static_cast<ULONGLONG>(Mft::Idx::Root), record);
}

std::span<BYTE> MftRecordBytes(std::vector<BYTE>& image, ULONGLONG idx) {
  const size_t offset =
      mft_address +
      static_cast<size_t>(fake_file_record_size) * gsl::narrow<size_t>(idx);
  return std::span<BYTE>(image).subspan(offset, fake_file_record_size);
}

NTFS_BROWSER_NAMESPACE::Data::HeaderResident&
    InitResidentAttr(FakeRecord& record, DWORD offset, Attr::Type type) {
  auto& attr = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderResident*>(
      &record.at(offset));
  attr.header.type = type;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.attr_offset = static_cast<WORD>(sizeof(attr));
  return attr;
}

NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident&
    InitNonResidentAttr(FakeRecord& record, DWORD offset, Attr::Type type) {
  auto& attr =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderNonResident*>(
          &record.at(offset));
  attr.header.type = type;
  attr.header.non_resident = 1;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  return attr;
}

DWORD WriteSingleRunList(std::span<BYTE> data_run, DWORD clusters, DWORD lcn) {
  DWORD run_len = 0;
  // High nibble = LCN offset field size, low nibble = length field size.
  gsl::at(data_run, run_len++) = run_header4_lcn_bytes;
  gsl::at(data_run, run_len++) = gsl::narrow<BYTE>(clusters);
  std::memcpy(&gsl::at(data_run, run_len), &lcn, sizeof(lcn));
  run_len += sizeof(lcn);
  gsl::at(data_run, run_len++) = 0x00;  // terminate the run list
  return run_len;
}

// Builds a fake $MFT record whose DATA attribute has a real, non-empty
// data run of clusters clusters starting at lcn, unlike MakeMftRecord()'s
// empty one.
FakeRecord MakeMftRecordWithRealDataRun(DWORD lcn, DWORD clusters) {
  FakeRecord record = MakeInUseFileRecord();

  auto& attr = InitNonResidentAttr(record, attr_offset_value, Attr::Type::Data);
  attr.start_vcn = 0;
  attr.last_vcn = clusters - 1;
  attr.data_run_offset = static_cast<WORD>(sizeof(attr));
  attr.comp_unit_size = 0;
  attr.real_size = ULONGLONG{clusters} * cluster_size;
  attr.alloc_size = attr.real_size;
  attr.ini_size = attr.real_size;

  const DWORD run_len = WriteSingleRunList(
      std::span<BYTE>(record).subspan(attr_offset_value + attr.data_run_offset),
      clusters, lcn);

  attr.header.total_size = AlignAttrSize(sizeof(attr) + run_len);

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Writes one resident $STANDARD_INFORMATION attribute at record[offset]
// with the given DOS permission bits, and returns its total_size.
DWORD WriteStandardInformationAttr(
    FakeRecord& record, DWORD offset,
    NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission permission) {
  auto& attr =
      InitResidentAttr(record, offset, Attr::Type::StandardInformation);
  attr.attr_size = static_cast<DWORD>(
      sizeof(NTFS_BROWSER_NAMESPACE::Data::StandardInformation));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& std_info =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::StandardInformation*>(
          &record.at(offset + attr.attr_offset));
  std_info.create_time = std_info_create_time;
  std_info.alter_time = std_info_alter_time;
  std_info.mft_time = std_info_mft_time;
  std_info.read_time = std_info_read_time;
  std_info.permission = permission;

  return attr.header.total_size;
}

// Writes one resident $FILE_NAME at record[offset] and returns its
// total_size.
DWORD WriteFileNameAttr(FakeRecord& record, DWORD offset,
                        const FakeFileName& name, bool directory) {
  auto& attr = InitResidentAttr(record, offset, Attr::Type::FileName);
  // On-disk units: a character past the BMP takes two, whatever wchar_t is.
  const std::vector<WORD> units = ToUtf16(name.name);
  attr.attr_size = gsl::narrow<DWORD>(
      offsetof(NTFS_BROWSER_NAMESPACE::Data::Filename, name) +
      units.size() * sizeof(WORD));
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  auto& filename = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::Filename*>(
      &record.at(offset + attr.attr_offset));
  filename.parent_ref = name.parent_ref;
  filename.real_size = name.real_size;
  filename.alloc_size = name.real_size;
  filename.flags =
      (directory ? NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::Directory
                 : NTFS_BROWSER_NAMESPACE::Data::FilenameFlag::None) |
      name.extra_flags;
  filename.name_length = gsl::narrow<BYTE>(units.size());
  filename.name_space = name.name_space;
  for (size_t i = 0; i < units.size(); i++) {
    filename.name[i] = units.at(i);
  }

  return attr.header.total_size;
}

// Writes one resident, unnamed $DATA of size zero bytes at record[offset]
// and returns its total_size.
DWORD WriteResidentDataAttr(FakeRecord& record, DWORD offset, DWORD size) {
  auto& attr = InitResidentAttr(record, offset, Attr::Type::Data);
  attr.attr_size = size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);
  return attr.header.total_size;
}

// Builds one record of BuildFakeNtfsImageWithMftTree(): a header carrying
// sequence and baseRef, a $STANDARD_INFORMATION, names, and, when dataSize is
// set, a resident $DATA of that size.
FakeRecord MakeMftTreeRecord(
    NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag flags, WORD sequence,
    std::initializer_list<FakeFileName> names, std::optional<DWORD> data_size,
    NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission permission,
    ULONGLONG base_ref) {
  FakeRecord record = MakeRecordHeader(attr_offset_value, flags, sequence);
  EditFileRecordHeader(
      record, [&](FileRecordHeader& header) { header.ref_to_base = base_ref; });

  const bool directory =
      (flags & NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir) ==
      NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag::Dir;

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

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
