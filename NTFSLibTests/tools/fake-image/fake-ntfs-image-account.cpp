#include <cstddef>
#include <cstdint>
#include <vector>

#include <ntfs-browser/namespace.h>

#include "data/attribute-list.h"
#include "data/file-record-flag.h"
#include "data/file-record-header.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "data/std-info-permission.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// The flag of an attribute header that marks its stream as encrypted.
constexpr WORD encrypted_attr_flag = 0x4000;

// Byte that ends a run list early: its length field claims 15 bytes, which no
// run can have, so a decoder stops there.
constexpr BYTE malformed_run_header = 0x1F;

// The on-disk run list of the first count runs, ended by a byte no run can
// start with. A real run has a 1-byte length and a 4-byte LCN delta.
std::vector<BYTE> CutRunList(const std::vector<FakeDataRun>& runs,
                             std::size_t count) {
  constexpr BYTE sparse_run_header = 0x01;
  std::vector<BYTE> bytes;
  ULONGLONG previous = 0;
  for (std::size_t i = 0; i < count && i < runs.size(); i++) {
    const FakeDataRun& run = runs.at(i);
    if (!run.lcn) {
      bytes.push_back(sparse_run_header);
      bytes.push_back(static_cast<BYTE>(run.clusters));
      continue;
    }
    bytes.push_back(run_header4_lcn_bytes);
    bytes.push_back(static_cast<BYTE>(run.clusters));
    const auto delta =
        static_cast<std::uint32_t>(static_cast<std::int64_t>(*run.lcn) -
                                   static_cast<std::int64_t>(previous));
    for (unsigned shift = 0; shift < 32; shift += 8) {
      bytes.push_back(static_cast<BYTE>((delta >> shift) & 0xFFU));
    }
    previous = *run.lcn;
  }
  bytes.push_back(malformed_run_header);
  return bytes;
}

// Writes the non-resident attributes streams at record[offset] on, and returns
// the offset after the last.
DWORD WriteStreams(FakeRecord& record, DWORD offset,
                   const std::vector<FakeAccountStream>& streams) {
  for (const FakeAccountStream& stream : streams) {
    FakeNonResidentOverrides overrides{.name = stream.name,
                                       .ini_size = stream.ini_size};
    if (stream.cut_after_runs) {
      overrides.raw_runs = CutRunList(stream.runs, *stream.cut_after_runs);
    }
    if (stream.encrypted) {
      overrides.flags = encrypted_attr_flag;
    }
    offset += WriteNonResidentAttr(record, offset, stream.type,
                                   {.comp_unit_size = stream.comp_unit_size,
                                    .real_size = stream.real_size,
                                    .runs = stream.runs,
                                    .overrides = overrides});
  }
  return offset;
}

// Writes a resident $ATTRIBUTE_LIST at record[offset] with one nameless entry
// for each of streams, in the record ext_idx, and returns its total_size.
DWORD WriteExtensionList(FakeRecord& record, DWORD offset, ULONGLONG ext_idx,
                         const std::vector<FakeAccountStream>& streams) {
  constexpr WORD entry_size = static_cast<WORD>(AlignAttrSize(
      NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size));
  auto& attr = InitResidentAttr(record, offset, Attr::Type::AttributeList);
  attr.attr_size = static_cast<DWORD>(entry_size * streams.size());
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);
  for (std::size_t i = 0; i < streams.size(); i++) {
    auto& entry =
        *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::AttributeList*>(
            &record.at(offset + attr.attr_offset + (i * entry_size)));
    entry.attr_type = streams.at(i).type;
    entry.record_size = entry_size;
    entry.name_length = 0;
    entry.name_offset = 0;
    entry.start_vcn = 0;
    entry.base_ref.segment_number = ext_idx;
    entry.base_ref.sequence_number = 0;
    entry.attr_id = 0;
  }
  return attr.header.total_size;
}

}  // namespace

void PutAccountFile(std::vector<BYTE>& image, ULONGLONG idx,
                    const FakeAccountFile& file) {
  using NTFS_BROWSER_NAMESPACE::Data::FileRecordFlag;
  FakeRecord record = MakeRecordHeader(
      attr_offset_value, file.freed ? FileRecordFlag{} : FileRecordFlag::InUse,
      file.sequence);
  EditFileRecordHeader(
      record, [&](Data::FileRecordHeader& header) { header.lsn = file.lsn; });

  const std::wstring name = L"account" + std::to_wstring(idx) + L".bin";
  DWORD offset = attr_offset_value;
  offset += WriteStandardInformationAttr(
      record, offset, NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Normal);
  offset += WriteFileNameAttr(
      record, offset,
      {.name = name, .parent_ref = MakeFileReference(mft_tree_docs_idx, 1)},
      false);
  if (file.extension_idx) {
    offset += WriteExtensionList(record, offset, *file.extension_idx,
                                 file.extension_streams);
  }
  offset = WriteStreams(record, offset, file.streams);
  WriteEndOfAttributesMarker(record, offset);
  PutMftRecord(image, idx, record);

  if (file.extension_idx) {
    FakeRecord extension = MakeRecordHeader(
        attr_offset_value,
        file.freed ? FileRecordFlag{} : FileRecordFlag::InUse, file.sequence);
    SetRecordLink(extension, file.sequence,
                  MakeFileReference(idx, file.sequence));
    const DWORD end =
        WriteStreams(extension, attr_offset_value, file.extension_streams);
    WriteEndOfAttributesMarker(extension, end);
    PutMftRecord(image, *file.extension_idx, extension);
  }
}

void SetRecordLsn(std::vector<BYTE>& image, ULONGLONG idx, ULONGLONG lsn) {
  EditFileRecordHeader(
      MftRecordBytes(image, idx),
      [lsn](Data::FileRecordHeader& header) { header.lsn = lsn; });
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
