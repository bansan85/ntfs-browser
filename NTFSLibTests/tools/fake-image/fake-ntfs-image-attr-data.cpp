#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gsl/narrow>

#include <ntfs-browser/mft/idx.h>
#include <ntfs-browser/namespace.h>

#include "data/attribute-list.h"
#include "data/file-record-header.h"
#include "data/header-non-resident.h"
#include "data/header-resident.h"
#include "fake-ntfs-image-internal.h"
#include "fake-ntfs-image.h"
#include "file-record-header-edit.h"
#include "named-stream-probe.h"
#include "ntfs-browser/attr/header-common.h"
#include "ntfs-browser/attr/type.h"
#include "ntfs-browser/win-types.h"

namespace NTFS_BROWSER_NAMESPACE::Tests {

namespace {

// A byte no fixture otherwise holds, to tell a forged one from the original.
constexpr BYTE forged_byte = 0xFF;

FakeRecord MakeUndersizedResidentAttrRecord() {
  FakeRecord record = MakeInUseFileRecord();

  constexpr DWORD undersized_total_size = 17;
  static_assert(undersized_total_size <
                    sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderResident),
                "total_size must be smaller than a resident attribute header");

  // Not InitResidentAttr(): the value offset is never written, so it stays 0.
  auto& attr = *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::HeaderResident*>(
      &record.at(attr_offset_value));
  attr.header.type = Attr::Type::ReparsePoint;
  attr.header.non_resident = 0;
  attr.header.name_length = 0;
  attr.header.flags = 0;
  attr.header.id = 0;
  attr.header.total_size = undersized_total_size;

  record.at(attr_offset_value +
            sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderResident)) = forged_byte;

  return record;
}

// A single resident $DATA attribute whose name_offset/name_length point
// past its own declared total_size, while still landing on known,
// deterministic bytes inside the record buffer.
FakeRecord MakeAttrNameExceedsTotalSizeRecord() {
  FakeRecord record = MakeInUseFileRecord();

  constexpr DWORD body_size = 4;

  auto& attr = InitResidentAttr(record, attr_offset_value, Attr::Type::Data);
  attr.attr_size = body_size;
  attr.header.total_size = AlignAttrSize(sizeof(attr) + body_size);

  static_assert(static_cast<DWORD>(attr_name_bounds_name_offset) +
                        2 * static_cast<DWORD>(attr_name_bounds_name_length) >
                    sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderResident) +
                        body_size,
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
  FakeRecord record = MakeInUseFileRecord();

  auto& attr = InitResidentAttr(record, attr_offset_value, Attr::Type::Data);
  attr.attr_size = gsl::narrow<DWORD>(small_resident_data_content.size());
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  std::memcpy(&record.at(attr_offset_value + attr.attr_offset),
              small_resident_data_content.data(),
              small_resident_data_content.size());

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);
  return record;
}

// Builds a root-directory replacement whose sole attribute is a resident,
// named $DATA stream (an ADS) holding named_data_stream_content under
// named_data_stream_name.
FakeRecord MakeNamedDataStreamRecord() {
  FakeRecord record = MakeInUseFileRecord();

  auto& attr = InitResidentAttr(record, attr_offset_value, Attr::Type::Data);
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

}  // namespace

std::vector<BYTE> BuildFakeNtfsImageWithUndersizedAttribute() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  PutMftRecord(image, undersized_attr_record_idx,
               MakeUndersizedResidentAttrRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttrNameExceedsTotalSize() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  PutMftRecord(image, attr_name_exceeds_total_size_record_idx,
               MakeAttrNameExceedsTotalSizeRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithAttrOffsetOutOfBounds() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // offset_of_attr is per-record, unlike the BPB fields patched above.
  EditFileRecordHeader(
      MftRecordBytes(image, static_cast<ULONGLONG>(Mft::Idx::Root)),
      [](FileRecordHeader& header) {
        header.offset_of_attr = attr_offset_out_of_bounds;
      });

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSmallResidentData() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  PutRootRecord(image, MakeSmallResidentDataRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithNamedDataStream() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  // Overwrites the whole root record (#5), not just a single field.
  PutRootRecord(image, MakeNamedDataStreamRecord());

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMinimalNonResidentData() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  FakeRecord record = MakeInUseFileRecord();

  auto& attr = InitNonResidentAttr(record, attr_offset_value, Attr::Type::Data);
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
  attr.data_run_offset = static_cast<WORD>(
      NTFS_BROWSER_NAMESPACE::Data::header_non_resident_base_size);
  attr.header.total_size =
      NTFS_BROWSER_NAMESPACE::Data::header_non_resident_base_size;

  WriteEndOfAttributesMarker(record,
                             attr_offset_value + attr.header.total_size);

  PutRootRecord(image, record);

  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithBadDataRun() {
  // Two real 1-cluster runs (VCN 0, then VCN 1 if both were accepted), but
  // last_vcn is forged to 0: the second run's own last_vcn (1) then exceeds
  // it, mid-list.
  const std::vector<FakeDataRun> runs{{bad_data_run_first_lcn, 1},
                                      {bad_data_run_second_lcn, 1}};

  const FakeRecord record = MakeNonResidentDataRecord(
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive, 0, cluster_size,
      runs, {.last_vcn = 0});

  std::vector<BYTE> image = BuildFakeNtfsImage();
  PutRootRecord(image, record);
  return image;
}

namespace {

// Fixed on-disk size of a nameless $ATTRIBUTE_LIST entry.
constexpr WORD list_entry_size = static_cast<WORD>(AlignAttrSize(
    NTFS_BROWSER_NAMESPACE::Data::attribute_list_entry_header_size));

// LCNs the lifetime fixtures use for cluster data: past the $MFT records
// (LCN 1 onwards) and distinct, so no two streams share a cluster.
constexpr DWORD lifetime_base_data_lcn = 50;

constexpr DWORD lifetime_ext_data_lcn = 51;

constexpr DWORD lifetime_list_lcn = 52;

// One nameless $ATTRIBUTE_LIST entry, as WriteListEntry() writes it.
struct FakeListEntry {
  // Attribute type the entry names.
  Attr::Type type;
  // Record the entry points to.
  ULONGLONG record;
  // record_size to declare. Zero is the forged value.
  WORD record_size;
  // Sequence number the entry claims for record.
  WORD sequence = 0;
};

// Writes one nameless $ATTRIBUTE_LIST entry at dest.
void WriteListEntry(BYTE* dest, const FakeListEntry& spec) {
  auto& entry =
      *reinterpret_cast<NTFS_BROWSER_NAMESPACE::Data::AttributeList*>(dest);
  entry.attr_type = spec.type;
  entry.record_size = spec.record_size;
  entry.name_length = 0;
  entry.name_offset = 0;
  entry.start_vcn = 0;
  entry.base_ref.segment_number = spec.record;
  entry.base_ref.sequence_number = spec.sequence;
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
  auto& attr = InitResidentAttr(record, offset, Attr::Type::AttributeList);
  attr.attr_size = gsl::narrow<DWORD>(list_entry_size * entry_count);
  attr.header.total_size = AlignAttrSize(sizeof(attr) + attr.attr_size);

  for (size_t i = 0; i < entry_count; i++) {
    WriteListEntry(
        &record.at(offset + attr.attr_offset + (i * list_entry_size)),
        {.type = Attr::Type::Data,
         .record = attr_list_lifetime_ext_idx,
         .record_size =
             (zero_size_index == i) ? static_cast<WORD>(0) : list_entry_size,
         .sequence = entry_sequence});
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
  FakeRecord record = MakeInUseFileRecord();
  SetRecordLink(record, link.record_sequence, link.base_ref);

  DWORD offset = attr_offset_value;
  const DWORD data_offset = offset;
  offset += WriteResidentDataAttr(record, offset,
                                  attr_list_lifetime_data_content.size());
  std::memcpy(&record.at(data_offset +
                         sizeof(NTFS_BROWSER_NAMESPACE::Data::HeaderResident)),
              attr_list_lifetime_data_content.data(),
              attr_list_lifetime_data_content.size());

  WriteEndOfAttributesMarker(record, offset);
  return record;
}

}  // namespace

std::vector<BYTE>
    BuildFakeNtfsImageWithAttributeListImportThenZeroRecordSize() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  FakeRecord base = MakeInUseFileRecord();
  DWORD offset = attr_offset_value;
  offset += WriteLifetimeResidentList(base, offset, 2, 1);
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, attr_list_lifetime_base_idx, base);
  PutMftRecord(image, attr_list_lifetime_ext_idx,
               MakeLifetimeResidentDataExtensionRecord());
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSplitAttributeListAttribute() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  FakeRecord base = MakeInUseFileRecord();
  DWORD offset = attr_offset_value;
  // VCN 0-0: empty.
  offset +=
      WriteNonResidentAttr(base, offset, Attr::Type::AttributeList,
                           {.comp_unit_size = 0, .real_size = 0, .runs = {}});
  // VCN 1-1: one entry, stored in its own cluster.
  const std::vector<FakeDataRun> runs{{lifetime_list_lcn, 1}};
  offset += WriteNonResidentAttr(base, offset, Attr::Type::AttributeList,
                                 {.comp_unit_size = 0,
                                  .real_size = list_entry_size,
                                  .runs = runs,
                                  .overrides = {.start_vcn = 1}});
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, attr_list_lifetime_base_idx, base);
  PutMftRecord(image, attr_list_lifetime_ext_idx,
               MakeLifetimeResidentDataExtensionRecord());

  std::vector<BYTE> entry(list_entry_size, 0);
  WriteListEntry(entry.data(), {.type = Attr::Type::Data,
                                .record = attr_list_lifetime_ext_idx,
                                .record_size = list_entry_size});
  LayRunBytes(image, runs, entry);
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithMultiClusterAttributeList() {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const std::vector<BYTE> list = [] {
    std::vector<BYTE> bytes(multi_cluster_list_entries * list_entry_size, 0);
    // The first entry imports the extension's $DATA, the others name the
    // record itself, as the entries of its own attributes do.
    for (size_t i = 0; i < multi_cluster_list_entries; i++) {
      WriteListEntry(
          &bytes.at(i * list_entry_size),
          {.type = i == 0 ? Attr::Type::Data : Attr::Type::StandardInformation,
           .record = i == 0 ? attr_list_lifetime_ext_idx
                            : attr_list_lifetime_base_idx,
           .record_size = list_entry_size});
    }
    return bytes;
  }();
  const std::vector<FakeDataRun> runs{
      {multi_cluster_list_lcn, multi_cluster_list_clusters}};

  FakeRecord base = MakeInUseFileRecord();
  DWORD offset = attr_offset_value;
  offset += WriteNonResidentAttr(
      base, offset, Attr::Type::AttributeList,
      {.comp_unit_size = 0, .real_size = list.size(), .runs = runs});
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, attr_list_lifetime_base_idx, base);
  PutMftRecord(image, attr_list_lifetime_ext_idx,
               MakeLifetimeResidentDataExtensionRecord());
  LayRunBytes(image, runs, list);
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithSplitDataAndTrailingDefect(
    FakeTrailingDefect defect) {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  const std::vector<FakeDataRun> base_runs{{lifetime_base_data_lcn, 1}};
  const std::vector<FakeDataRun> ext_runs{{lifetime_ext_data_lcn, 1}};

  FakeRecord base = MakeInUseFileRecord();
  DWORD offset = attr_offset_value;
  offset += WriteLifetimeResidentList(base, offset, 1);
  offset += WriteNonResidentAttr(
      base, offset, Attr::Type::Data,
      {.comp_unit_size = 0, .real_size = cluster_size, .runs = base_runs});
  WriteTrailingDefect(base, offset, defect);

  FakeRecord ext = MakeInUseFileRecord();
  SetRecordLink(ext, 0, attr_list_lifetime_base_idx);
  DWORD ext_offset = attr_offset_value;
  ext_offset += WriteNonResidentAttr(ext, ext_offset, Attr::Type::Data,
                                     {.comp_unit_size = 0,
                                      .real_size = 0,
                                      .runs = ext_runs,
                                      .overrides = {.start_vcn = 1}});
  WriteEndOfAttributesMarker(ext, ext_offset);

  PutMftRecord(image, attr_list_lifetime_base_idx, base);
  PutMftRecord(image, attr_list_lifetime_ext_idx, ext);
  LayRunBytes(image, base_runs, {});
  LayRunBytes(image, ext_runs, {});
  return image;
}

std::vector<BYTE> BuildFakeNtfsImageWithExtensionLink(FakeExtensionLink link) {
  std::vector<BYTE> image = BuildFakeNtfsImage();

  FakeRecord base = MakeInUseFileRecord();
  DWORD offset = attr_offset_value;
  offset += WriteLifetimeResidentList(base, offset, 1, {}, link.entry_sequence);
  WriteEndOfAttributesMarker(base, offset);

  PutMftRecord(image, attr_list_lifetime_base_idx, base);
  PutMftRecord(image, attr_list_lifetime_ext_idx,
               MakeLifetimeResidentDataExtensionRecord(link));
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
  const auto permission =
      NTFS_BROWSER_NAMESPACE::Data::StdInfoPermission::Archive;

  const FakeRecord record =
      (host == FakeRunHost::Data)
          ? MakeNonResidentDataRecord(permission, 0, real_size, placeholder,
                                      overrides)
          : MakeIndexAllocationDirRecord(permission, {.comp_unit_size = 0,
                                                      .real_size = real_size,
                                                      .runs = placeholder,
                                                      .overrides = overrides});
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

std::vector<BYTE> BuildFakeNtfsImageWithNegativeLcn(FakeRunHost host) {
  // Two's-complement encoding of the LCN delta -1, in one offset byte.
  constexpr BYTE minus_one_byte = 0xFF;
  std::vector<BYTE> runs{run_header1_lcn_bytes, 1, minus_one_byte};
  return BuildImageWithRawRuns(host, std::move(runs), 1);
}

}  // namespace NTFS_BROWSER_NAMESPACE::Tests
